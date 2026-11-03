"""Ubuntu 24.04 installer for the isolated stubuddy IP-certificate WSS checkpoint.

Run --check first. Does not alter security groups, UFW, SSH or other applications.
"""
import argparse
import datetime
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import subprocess

SOURCE = Path(__file__).resolve().parent
APP = Path("/opt/stubuddy-echo")
SETTINGS = Path("/etc/stubuddy/echo.json")
NGINX = Path("/etc/nginx/conf.d/stubuddy-echo.conf")
ACME = Path("/etc/stubuddy-acme")
WEBROOT = Path("/var/www/stubuddy-acme")
CERTBOT = "/opt/stubuddy-certbot/bin/certbot"
RELOAD = Path("/usr/local/libexec/stubuddy-reload-nginx")
CERT = ACME / "live/stubuddy-ip/fullchain.pem"
KEY = ACME / "live/stubuddy-ip/privkey.pem"
CODE = ("echo_server.py", "echo_protocol.py", "requirements.txt")


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def load_config():
    config = json.loads((SOURCE / ".cloud.json").read_text(encoding="utf-8"))
    ip = ipaddress.IPv4Address(config["host"])
    if (not ip.is_global or ip.is_multicast or config.get("port") != 8765 or
            config.get("url") != f"wss://{ip}/stubuddy/echo" or
            not isinstance(config.get("token"), str) or
            not re.fullmatch(r"[a-f0-9]{64}", config["token"])):
        raise ValueError("Invalid cloud profile; regenerate with configure_cloud.py")
    return config


def preflight(config):
    release = Path("/etc/os-release").read_text(encoding="utf-8")
    if not re.search(r'^ID="?ubuntu"?$', release, re.M) or not re.search(r'^VERSION_ID="24\.04"$', release, re.M):
        raise ValueError("This checkpoint installer requires Ubuntu 24.04")
    marker = APP / ".managed-by-stubuddy"
    managed = marker.is_file() and marker.read_text().strip() == config["host"]
    if APP.exists() and not managed:
        raise ValueError("Existing /opt/stubuddy-echo is unmanaged or uses another IP; preserve it")
    owned_paths = (SETTINGS, NGINX, ACME, Path(str(ACME) + "-staging"), RELOAD,
                   Path("/opt/stubuddy-certbot"), WEBROOT,
                   Path("/etc/systemd/system/stubuddy-echo.service"),
                   Path("/etc/systemd/system/stubuddy-echo.service.d/test-delay.conf"),
                   Path("/etc/systemd/system/stubuddy-cert-renew.service"),
                   Path("/etc/systemd/system/stubuddy-cert-renew.timer"))
    if not managed:
        for path in owned_paths:
            if path.exists():
                raise ValueError(f"Existing unmanaged deployment path: {path}; preserve it")
    if not shutil.which("ss"):
        raise ValueError("Install iproute2 before preflight")
    listeners = subprocess.check_output(["ss", "-H", "-ltnp"], text=True)
    active = subprocess.run(["systemctl", "is-active", "--quiet", "stubuddy-echo.service"]).returncode == 0
    for line in listeners.splitlines():
        fields = line.split()
        port = fields[3].rsplit(":", 1)[-1]
        if port in ("80", "443") and not (managed and '"nginx"' in line):
            raise ValueError(f"Port {port} already in use; do not replace the existing service")
        if port == "8765" and not (managed and active and fields[3] == "127.0.0.1:8765"):
            raise ValueError("Port 8765 already in use by another service")
    print("CHECK PASS: Ubuntu 24.04; dedicated paths and ports available/owned")
    print("REQUIRED: public TCP 80 (ACME) and 443 (WSS); do not open 8765")
    print("Security groups/UFW/SSH are not changed by this installer.")


def nginx_config(ip, tls):
    http = f"""# Managed by stubuddy checkpoint 3.6.2
server {{
    listen 80;
    server_name {ip};
    access_log off;
    location ^~ /.well-known/acme-challenge/ {{
        root {WEBROOT};
        default_type text/plain;
        try_files $uri =404;
    }}
    location / {{ return 404; }}
}}
"""
    if not tls:
        return http
    return http + f"""server {{
    listen 443 ssl;
    server_name {ip};
    ssl_certificate {CERT};
    ssl_certificate_key {KEY};
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_session_cache shared:stubuddy_tls:1m;
    ssl_session_timeout 10m;
    access_log off;
    error_log /var/log/nginx/stubuddy-echo-error.log warn;
    client_max_body_size 16k;
    location = /healthz {{ default_type text/plain; return 200 "stubuddy TLS entry ready\\n"; }}
    location = /stubuddy/echo {{
        proxy_pass http://127.0.0.1:8765;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_buffering off;
        proxy_read_timeout 120s;
        proxy_send_timeout 120s;
    }}
    location / {{ return 404; }}
}}
"""


def write(path, content, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".stubuddy-new")
    temporary.write_text(content, encoding="utf-8")
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def reload_nginx(content):
    previous = NGINX.read_text() if NGINX.exists() else None
    write(NGINX, content)
    try:
        run("nginx", "-t")
    except subprocess.CalledProcessError:
        if previous is None:
            NGINX.unlink()
        else:
            write(NGINX, previous)
        raise
    run("systemctl", "enable", "--now", "nginx")
    run("systemctl", "reload", "nginx")


def certbot_paths(staging=False):
    suffix = "-staging" if staging else ""
    return ["--config-dir", str(ACME) + suffix,
            "--work-dir", "/var/lib/stubuddy-acme" + suffix,
            "--logs-dir", "/var/log/stubuddy-acme" + suffix]


def install(config, email):
    if os.geteuid() != 0:
        raise ValueError("Use sudo python3 install_ecs.py --install --email YOUR_EMAIL")
    if not re.fullmatch(r"[^\s@]+@[^\s@]+\.[^\s@]+", email or ""):
        raise ValueError("Provide an ACME account contact email with --email")
    os.umask(0o022)  # Runtime code/venv readable; private files have explicit modes below.
    # Back up only our own configuration; never copy other services or private keys.
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    backup = Path("/var/backups/stubuddy-echo") / stamp
    backup.mkdir(parents=True, mode=0o700)
    os.chmod(backup.parent, 0o700)
    for index, path in enumerate((SETTINGS, NGINX,
                                 Path("/etc/systemd/system/stubuddy-echo.service"),
                                 Path("/etc/systemd/system/stubuddy-cert-renew.service"),
                                 Path("/etc/systemd/system/stubuddy-cert-renew.timer"),
                                 Path("/etc/systemd/system/stubuddy-echo.service.d/test-delay.conf"))):
        if path.exists():
            shutil.copy2(path, backup / f"{index}-{path.name}")
    run("apt-get", "update")
    run("apt-get", "install", "-y", "nginx", "python3-venv", "ca-certificates", "openssl")
    if subprocess.run(["id", "-u", "stubuddy-echo"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode:
        run("useradd", "--system", "--user-group", "--no-create-home", "--shell", "/usr/sbin/nologin", "stubuddy-echo")
    APP.mkdir(parents=True, exist_ok=True)
    os.chmod(APP, 0o755)
    write(APP / ".managed-by-stubuddy", config["host"] + "\n")
    run("python3", "-m", "venv", str(APP / "venv"))
    for name in CODE:
        write(APP / name, (SOURCE / name).read_text(encoding="utf-8"))
    run(str(APP / "venv/bin/python"), "-m", "pip", "install", "-r", str(APP / "requirements.txt"))
    write(SETTINGS, json.dumps(config, indent=2) + "\n", 0o640)
    shutil.chown(SETTINGS, user="root", group="stubuddy-echo")
    write(Path("/etc/systemd/system/stubuddy-echo.service"), """[Unit]
Description=stubuddy private RAM-only echo
After=network-online.target
Wants=network-online.target
[Service]
User=stubuddy-echo
Group=stubuddy-echo
WorkingDirectory=/opt/stubuddy-echo
ExecStart=/opt/stubuddy-echo/venv/bin/python /opt/stubuddy-echo/echo_server.py --host 127.0.0.1 --config /etc/stubuddy/echo.json
Environment=PYTHONDONTWRITEBYTECODE=1
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
RestrictSUIDSGID=true
UMask=0077
MemoryMax=192M
MemorySwapMax=0
TasksMax=32
LimitNOFILE=256
LogRateLimitIntervalSec=30s
LogRateLimitBurst=200
[Install]
WantedBy=multi-user.target
""")
    override = Path("/etc/systemd/system/stubuddy-echo.service.d/test-delay.conf")
    if override.exists():
        write(override, "[Service]\n")  # A normal install clears its own test delay.
    WEBROOT.mkdir(parents=True, exist_ok=True, mode=0o755)
    os.chmod(WEBROOT, 0o755)
    reload_nginx(nginx_config(config["host"], CERT.is_file()))
    run("python3", "-m", "venv", "/opt/stubuddy-certbot")
    run("/opt/stubuddy-certbot/bin/python", "-m", "pip", "install", "certbot==5.9.0")
    write(RELOAD, "#!/bin/sh\nset -eu\n/usr/sbin/nginx -t\n/bin/systemctl reload nginx\n", 0o755)
    base = [CERTBOT, "certonly", "--config", "/dev/null", "--webroot", "--webroot-path", str(WEBROOT),
            "--ip-address", config["host"], "--cert-name", "stubuddy-ip",
            "--preferred-profile", "shortlived", "--agree-tos", "--non-interactive", "--email", email]
    # Separate ACME staging state: a test certificate is never installed into Nginx.
    run(*(base + ["--staging"] + certbot_paths(True)))
    run(*(base + ["--deploy-hook", str(RELOAD)] + certbot_paths()))
    reload_nginx(nginx_config(config["host"], True))
    renewal = " ".join([CERTBOT, "renew", "--config", "/dev/null", "--quiet", "--no-random-sleep-on-renew", "--cert-name", "stubuddy-ip",
                        "--deploy-hook", str(RELOAD)] + certbot_paths())
    write(Path("/etc/systemd/system/stubuddy-cert-renew.service"), f"""[Unit]
Description=Renew stubuddy short-lived IP certificate
After=network-online.target nginx.service
Wants=network-online.target
[Service]
Type=oneshot
ExecStart={renewal}
ExecStartPost=/usr/bin/openssl x509 -in {CERT} -noout -checkend 86400
""")
    write(Path("/etc/systemd/system/stubuddy-cert-renew.timer"), """[Unit]
Description=Hourly stubuddy certificate renewal check
[Timer]
OnCalendar=hourly
RandomizedDelaySec=10m
Persistent=true
[Install]
WantedBy=timers.target
""")
    run("systemd-analyze", "verify", "/etc/systemd/system/stubuddy-echo.service",
        "/etc/systemd/system/stubuddy-cert-renew.service", "/etc/systemd/system/stubuddy-cert-renew.timer")
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "--now", "stubuddy-echo.service", "stubuddy-cert-renew.timer")
    run("systemctl", "restart", "stubuddy-echo.service")
    run("systemctl", "start", "stubuddy-cert-renew.service")
    run(*([CERTBOT, "renew", "--config", "/dev/null", "--dry-run", "--no-random-sleep-on-renew", "--run-deploy-hooks", "--cert-name", "stubuddy-ip",
            "--deploy-hook", str(RELOAD)] + certbot_paths()))
    with socket.create_connection(("127.0.0.1", 443), timeout=10) as raw:
        with ssl.create_default_context().wrap_socket(raw, server_hostname=config["host"]) as secured:
            secured.sendall(f"GET /healthz HTTP/1.1\r\nHost: {config['host']}\r\nConnection: close\r\n\r\n".encode("ascii"))
            if not secured.recv(1024).startswith(b"HTTP/1.1 200"):
                raise ValueError("Local verified TLS health check failed")
    print("INSTALL PASS: verified TLS entry, echo service, hourly renewal and renewal dry-run")
    print("Next: run smoke_test.py --cloud from Windows; then flash and verify hardware.")


def set_test_delay(seconds):
    if os.geteuid() != 0:
        raise ValueError("Use sudo for the owned echo test override")
    write(Path("/etc/systemd/system/stubuddy-echo.service.d/test-delay.conf"), f"""[Service]
ExecStart=
ExecStart=/opt/stubuddy-echo/venv/bin/python /opt/stubuddy-echo/echo_server.py --host 127.0.0.1 --config /etc/stubuddy/echo.json --echo-delay {seconds}
""")
    run("systemctl", "daemon-reload")
    run("systemctl", "restart", "stubuddy-echo.service")
    print(f"Echo test delay set to {seconds} seconds; service restarted")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="Read-only preflight")
    mode.add_argument("--install", action="store_true", help="Install isolated services after preflight")
    mode.add_argument("--test-delay", type=int, choices=range(0, 11), help="Restart owned echo service with a 0..10s test delay; restore with 0")
    parser.add_argument("--email", help="ACME account contact; not printed by this script")
    args = parser.parse_args()
    try:
        config = load_config()
        preflight(config)
        if args.install:
            install(config, args.email or config.get("email"))
        elif args.test_delay is not None:
            if not (APP / ".managed-by-stubuddy").is_file():
                raise ValueError("Install the owned ECS service first")
            set_test_delay(args.test_delay)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit("STOP: " + str(error)) from error
