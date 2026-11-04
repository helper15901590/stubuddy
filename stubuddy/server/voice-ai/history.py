"""Bounded sliding windows. Only device-confirmed playback commits a pair."""
from collections import deque
import time


class History:
    def __init__(self, rounds=8, budget=64000, idle_seconds=1800):
        if type(rounds) is not int or not 1 <= rounds <= 20:
            raise ValueError("history_rounds must be 1..20")
        if type(budget) is not int or not 2048 <= budget <= 64000:
            raise ValueError("history_bytes must be 2048..64000")
        self.pairs = deque(maxlen=rounds)
        self.rounds = rounds
        self.budget = budget
        self.idle_seconds = idle_seconds
        self.touched = time.monotonic()

    def expire(self):
        if time.monotonic() - self.touched >= self.idle_seconds:
            self.pairs.clear()
        self.touched = time.monotonic()

    def messages(self, prompt, query):
        self.expire()
        def cost(value):
            return len(value.encode("utf-8"))
        used = cost(prompt) + cost(query)
        if used > self.budget:
            raise ValueError("Current prompt/input exceeds history byte budget")
        # The eight slots contain completed history only. Current input is extra.
        # Q9 sees completed pairs 1..8; committing A9 slides storage to pairs 2..9.
        chosen = list(self.pairs)
        used += sum(cost(question) + cost(answer) for question, answer in chosen)
        if used > self.budget:
            raise ValueError("Context exceeds byte budget; the window is never silently shortened")
        messages = [{"role": "system", "content": prompt}]
        for question, answer in chosen:
            messages.extend([{"role": "user", "content": question},
                             {"role": "assistant", "content": answer}])
        messages.append({"role": "user", "content": query})
        return messages

    def commit(self, question, answer):
        self.expire()
        self.pairs.append((question, answer))
