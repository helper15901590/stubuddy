"""Cloud isolation, safe deployment preflight, and real verified TLS echo/negative tests."""
import contextlib
import datetime
import io
import ipaddress
import json
from pathlib import Path
import ssl
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT.parents[1] / "stubuddy/server/ws-echo"
sys.path.insert(0, str(SERVER))
import configure_cloud
import install_ecs
try:
    import smoke_test
    from echo_server import EchoService
    from websockets.asyncio.server import serve
    from websockets.exceptions import InvalidStatus
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
    TLS_AVAILABLE = True
except ImportError:
    TLS_AVAILABLE = False


class CloudConfigurationTests(unittest.TestCase):
    def test_cloud_profile_preserves_lan_and_cloud_tokens_and_packages_only_allowlist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in configure_cloud.PACKAGE_FILES:
                (root / name).write_text("# deployment source\n")
            (root / ".local.json").write_text('{"token":"LAN-untouched"}')
            firmware = root / "firmware"
            (firmware / "tmp").mkdir(parents=True)
            (firmware / "tmp/stubuddy_echo_local.h").write_text("LAN-untouched")
            output = io.StringIO()
            with mock.patch.object(configure_cloud, "ROOT", root), mock.patch.object(configure_cloud, "FIRMWARE", firmware), contextlib.redirect_stdout(output):
                configure_cloud.configure("121.41.86.40")
                first = json.loads((root / ".cloud.json").read_text())
                configure_cloud.configure("8.8.8.8")
                second = json.loads((root / ".cloud.json").read_text())
            self.assertEqual(first["token"], second["token"])
            self.assertNotIn(first["token"], output.getvalue())
            self.assertEqual((root / ".local.json").read_text(), '{"token":"LAN-untouched"}')
            self.assertEqual((firmware / "tmp/stubuddy_echo_local.h").read_text(), "LAN-untouched")
            self.assertIn("wss://8.8.8.8/stubuddy/echo", (firmware / "tmp/stubuddy_echo_cloud.h").read_text())
            with tarfile.open(root / ".cloud-deploy.tar.gz") as archive:
                self.assertEqual(set(archive.getnames()), {*configure_cloud.PACKAGE_FILES, ".cloud.json"})
                self.assertEqual(json.load(archive.extractfile(".cloud.json")), second)

    def test_invalid_inputs_do_not_replace_existing_cloud_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            settings = root / ".cloud.json"
            settings.write_text('{"token":"preserve-invalid-file"}')
            with mock.patch.object(configure_cloud, "ROOT", root):
                for address in ("192.168.4.1", "127.0.0.1", "0.0.0.0", "224.0.0.1", "121.41.86.40\nBAD", "121.41.86.40"):
                    with self.assertRaises(ValueError):
                        configure_cloud.configure(address)
            self.assertEqual(settings.read_text(), '{"token":"preserve-invalid-file"}')


class DeploymentSafetyTests(unittest.TestCase):
    def test_occupied_port_stops_preflight_before_install(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = Path.read_text
            def read(path, *args, **kwargs):
                if path.as_posix() == "/etc/os-release":
                    return 'ID=ubuntu\nVERSION_ID="24.04"\n'
                return original(path, *args, **kwargs)
            with mock.patch.object(install_ecs, "APP", root / "app"), mock.patch.object(install_ecs, "NGINX", root / "nginx.conf"), mock.patch.object(Path, "read_text", read), mock.patch.object(install_ecs.shutil, "which", return_value="ss"), mock.patch.object(install_ecs.subprocess, "check_output", return_value='LISTEN 0 128 0.0.0.0:443 0.0.0.0:* users:(("apache2",pid=1,fd=3))\n'), mock.patch.object(install_ecs.subprocess, "run", return_value=mock.Mock(returncode=3)):
                with self.assertRaisesRegex(ValueError, "443 already in use"):
                    install_ecs.preflight({"host": "121.41.86.40"})
            self.assertFalse((root / "app").exists())

    def test_failed_nginx_validation_restores_previous_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nginx.conf"
            path.write_text("previous valid config")
            with mock.patch.object(install_ecs, "NGINX", path), mock.patch.object(install_ecs, "run", side_effect=subprocess.CalledProcessError(1, ["nginx", "-t"])) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    install_ecs.reload_nginx("invalid next config")
            self.assertEqual(path.read_text(), "previous valid config")
            self.assertEqual(run.call_count, 1)  # No reload/restart after a failed check.


@unittest.skipUnless(TLS_AVAILABLE, "Install server requirements and use the IDF Python environment")
class WssTransportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.token = "a" * 64
        self.ca_key = ec.generate_private_key(ec.SECP256R1())
        self.ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "stubuddy ephemeral test CA")])
        now = datetime.datetime.now(datetime.timezone.utc)
        self.now = now
        ca = (x509.CertificateBuilder().subject_name(self.ca_name).issuer_name(self.ca_name)
              .public_key(self.ca_key.public_key()).serial_number(x509.random_serial_number())
              .not_valid_before(now - datetime.timedelta(days=2)).not_valid_after(now + datetime.timedelta(days=2))
              .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
              .add_extension(x509.SubjectKeyIdentifier.from_public_key(self.ca_key.public_key()), critical=False)
              .add_extension(x509.KeyUsage(digital_signature=False, key_encipherment=False, content_commitment=False, data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True, encipher_only=False, decipher_only=False), critical=True)
              .sign(self.ca_key, hashes.SHA256()))
        self.ca_path = self.root / "ca.pem"
        self.ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
        self.servers = []

    async def asyncTearDown(self):
        for server in self.servers:
            server.close()
            await server.wait_closed()
        self.directory.cleanup()

    async def target(self, ip="127.0.0.1", expired=False):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = (x509.CertificateBuilder()
                .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "test server")]))
                .issuer_name(self.ca_name).public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(self.now - datetime.timedelta(days=1))
                .not_valid_after(self.now + datetime.timedelta(hours=-1 if expired else 24))
                .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(self.ca_key.public_key()), critical=False)
                .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(ip))]), critical=False)
                .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
                .sign(self.ca_key, hashes.SHA256()))
        cert_path, key_path = self.root / "server.pem", self.root / "server.key"
        cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert_path, key_path)
        service = EchoService(self.token)
        server = await serve(service.handle, "127.0.0.1", 0, ssl=context, process_request=service.authenticate, compression=None)
        self.servers.append(server)
        return f"wss://127.0.0.1:{server.sockets[0].getsockname()[1]}/stubuddy/echo"

    async def smoke(self, url, trusted=True, token=None):
        (self.root / ".cloud.json").write_text(json.dumps({"url": url, "token": token or self.token}))
        context = ssl.create_default_context(cafile=str(self.ca_path) if trusted else None)
        with mock.patch.object(smoke_test, "ROOT", self.root), mock.patch.object(smoke_test.ssl, "create_default_context", return_value=context), contextlib.redirect_stdout(io.StringIO()):
            await smoke_test.main(cloud=True)

    async def test_verified_ip_certificate_round_trip(self):
        await self.smoke(await self.target())

    async def test_unknown_ca_is_rejected(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            await self.smoke(await self.target(), trusted=False)

    async def test_wrong_ip_identity_is_rejected(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            await self.smoke(await self.target(ip="127.0.0.2"))

    async def test_expired_certificate_is_rejected(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            await self.smoke(await self.target(expired=True))

    async def test_wrong_cloud_token_is_rejected_over_tls(self):
        with self.assertRaises(InvalidStatus) as error:
            await self.smoke(await self.target(), token="b" * 64)
        self.assertEqual(error.exception.response.status_code, 401)

    async def test_cloud_smoke_does_not_downgrade_to_plaintext(self):
        with self.assertRaisesRegex(ValueError, "requires WSS"):
            await self.smoke("ws://127.0.0.1:1/stubuddy/echo")


if __name__ == "__main__":
    unittest.main()
