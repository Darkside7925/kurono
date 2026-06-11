"""
Kurono OS — Core Kernel & Shell Engine
Central command registry, execution engine, environment switching, and interactive shell.
"""

import os
import sys
import time
import hashlib
import platform
import random
from typing import Dict, List, Optional, Callable, Any, Tuple


# ══════════════════════════════════════════════════════════════════════════════
#  Environment & Command Types
# ══════════════════════════════════════════════════════════════════════════════

ENV_LINUX   = "linux"
ENV_WINDOWS = "windows"
ENV_KURONO  = "kurono"
ALL_ENVS    = [ENV_LINUX, ENV_WINDOWS, ENV_KURONO]

class CommandEntry:
    """A registered command."""
    __slots__ = ("name", "env", "path", "description", "handler", "category", "aliases")

    def __init__(self, name: str, env: str, handler: Callable = None,
                 description: str = "", path: str = "", category: str = "general",
                 aliases: List[str] = None):
        self.name = name
        self.env = env
        self.handler = handler
        self.description = description
        self.path = path or f"/{env}/bin/{name}"
        self.category = category
        self.aliases = aliases or []


class Process:
    """A running process in the Kurono process table."""
    _next_pid = 1000

    def __init__(self, name: str, env: str, user: str = "user"):
        Process._next_pid += 1
        self.pid = Process._next_pid
        self.name = name
        self.env = env
        self.user = user
        self.started = time.time()
        self.cpu = round(random.uniform(0.0, 5.0), 1)
        self.mem = round(random.uniform(0.5, 80.0), 1)
        self.status = "running"  # running | sleeping | stopped | zombie

    def runtime(self) -> str:
        s = int(time.time() - self.started)
        return f"{s//3600:02d}:{(s%3600)//60:02d}:{s%60:02d}"

    def to_dict(self) -> dict:
        return {
            "pid": self.pid, "name": self.name, "env": self.env,
            "user": self.user, "cpu": self.cpu, "mem": self.mem,
            "status": self.status, "runtime": self.runtime(),
        }


# ══════════════════════════════════════════════════════════════════════════════
#  Kernel Core
# ══════════════════════════════════════════════════════════════════════════════

class KuronoKernel:
    """Central kernel: command registry, process table, env switching."""

    VERSION = "1.0.0"
    CODENAME = "Aurora"

    def __init__(self):
        self.current_env = ENV_KURONO
        self.registry: Dict[str, List[CommandEntry]] = {}  # cmd_name -> list of entries
        self.alias_map: Dict[str, str] = {}                # alias -> real name
        self.processes: Dict[int, Process] = {}
        self.env_variables: Dict[str, str] = {
            "USER": "user",
            "HOME": "/home/user",
            "PATH": "/bin:/sbin:/usr/bin:/usr/local/bin:/kurono/bin",
            "SHELL": "/bin/ksh",
            "TERM": "kurono-256color",
            "LANG": "en_US.UTF-8",
            "KURONO_VERSION": self.VERSION,
            "PS1": "kurono> ",
            "HOSTNAME": "kurono-machine",
            "EDITOR": "ked",
            "PWD": "/home/user",
        }
        self.history: List[str] = []
        self.log: List[Tuple[float, str, str]] = []  # (timestamp, level, message)
        self._boot_time = time.time()

        # Seed system processes
        self._spawn_system_processes()

    # ── Registration ──────────────────────────────────────────────────────────

    def register(self, entry: CommandEntry):
        self.registry.setdefault(entry.name, []).append(entry)
        for alias in entry.aliases:
            self.alias_map[alias] = entry.name

    def register_many(self, entries: List[CommandEntry]):
        for e in entries:
            self.register(e)

    def lookup(self, name: str) -> List[CommandEntry]:
        real = self.alias_map.get(name, name)
        return self.registry.get(real, [])

    def lookup_for_env(self, name: str, env: str = None) -> Optional[CommandEntry]:
        env = env or self.current_env
        for e in self.lookup(name):
            if e.env == env:
                return e
        entries = self.lookup(name)
        return entries[0] if entries else None

    def all_commands(self, env: str = None) -> List[CommandEntry]:
        cmds = []
        seen = set()
        for name, entries in self.registry.items():
            for e in entries:
                if env and e.env != env:
                    continue
                if e.name not in seen:
                    cmds.append(e)
                    seen.add(e.name)
        return sorted(cmds, key=lambda c: c.name)

    # ── Environment ───────────────────────────────────────────────────────────

    def switch_env(self, env: str) -> bool:
        if env not in ALL_ENVS:
            return False
        self.current_env = env
        self._log("info", f"Switched to {env} environment")
        return True

    def get_env(self) -> str:
        return self.current_env

    # ── Process table ─────────────────────────────────────────────────────────

    def _spawn_system_processes(self):
        system_procs = [
            ("kinit", ENV_KURONO, "root"),
            ("ksched", ENV_KURONO, "root"),
            ("kvfs", ENV_KURONO, "root"),
            ("knetd", ENV_KURONO, "root"),
            ("kwifi", ENV_KURONO, "root"),
            ("kgui", ENV_KURONO, "root"),
            ("kaudio", ENV_KURONO, "root"),
            ("klog", ENV_KURONO, "root"),
            ("linux-bridge", ENV_LINUX, "root"),
            ("windows-bridge", ENV_WINDOWS, "root"),
            ("supr-engine", ENV_KURONO, "root"),
            ("pkg-manager", ENV_KURONO, "root"),
        ]
        for pname, env, user in system_procs:
            p = Process(pname, env, user)
            p.status = "running"
            p.cpu = round(random.uniform(0.0, 1.5), 1)
            p.mem = round(random.uniform(0.2, 4.0), 1)
            self.processes[p.pid] = p

    def spawn(self, name: str, env: str = None, user: str = None) -> Process:
        env = env or self.current_env
        user = user or self.env_variables.get("USER", "user")
        p = Process(name, env, user)
        self.processes[p.pid] = p
        self._log("info", f"Process {name} (PID {p.pid}) spawned")
        return p

    def kill_process(self, pid: int) -> bool:
        if pid in self.processes:
            self.processes[pid].status = "stopped"
            self._log("info", f"Process PID {pid} killed")
            del self.processes[pid]
            return True
        return False

    def list_processes(self, env: str = None, user: str = None) -> List[Process]:
        procs = list(self.processes.values())
        if env:
            procs = [p for p in procs if p.env == env]
        if user:
            procs = [p for p in procs if p.user == user]
        return sorted(procs, key=lambda p: p.pid)

    # ── Env variables ─────────────────────────────────────────────────────────

    def set_var(self, key: str, value: str):
        self.env_variables[key] = value

    def get_var(self, key: str, default: str = "") -> str:
        return self.env_variables.get(key, default)

    def expand_vars(self, text: str) -> str:
        for key, val in self.env_variables.items():
            text = text.replace(f"${key}", val)
            text = text.replace(f"${{{key}}}", val)
        return text

    # ── History ───────────────────────────────────────────────────────────────

    def add_history(self, cmd: str):
        if cmd.strip():
            self.history.append(cmd.strip())
            if len(self.history) > 5000:
                self.history = self.history[-5000:]

    # ── Logging ───────────────────────────────────────────────────────────────

    def _log(self, level: str, message: str):
        self.log.append((time.time(), level, message))
        if len(self.log) > 10000:
            self.log = self.log[-10000:]

    def get_logs(self, last_n: int = 50) -> List[Tuple[float, str, str]]:
        return self.log[-last_n:]

    # ── System info ───────────────────────────────────────────────────────────

    def uptime(self) -> str:
        s = int(time.time() - self._boot_time)
        h, rem = divmod(s, 3600)
        m, sec = divmod(rem, 60)
        return f"{h}h {m}m {sec}s"

    def sysinfo(self) -> dict:
        return {
            "os": "Kurono OS",
            "version": self.VERSION,
            "codename": self.CODENAME,
            "kernel": f"kurono-kernel {self.VERSION}",
            "arch": platform.machine() or "x86_64",
            "hostname": self.env_variables.get("HOSTNAME", "kurono"),
            "uptime": self.uptime(),
            "user": self.env_variables.get("USER", "user"),
            "environment": self.current_env,
            "processes": len(self.processes),
            "host_platform": platform.system(),
        }

    def version_string(self) -> str:
        return f"Kurono OS {self.VERSION} \"{self.CODENAME}\""

    def motd(self) -> str:
        return (
            f"╔══════════════════════════════════════════════════════════════╗\n"
            f"║               ██╗  ██╗██╗   ██╗██████╗  ██████╗           ║\n"
            f"║               ██║ ██╔╝██║   ██║██╔══██╗██╔═══██╗          ║\n"
            f"║               █████╔╝ ██║   ██║██████╔╝██║   ██║          ║\n"
            f"║               ██╔═██╗ ██║   ██║██╔══██╗██║   ██║          ║\n"
            f"║               ██║  ██╗╚██████╔╝██║  ██║╚██████╔╝          ║\n"
            f"║               ╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝          ║\n"
            f"║                     Kurono OS {self.VERSION} \"{self.CODENAME}\"              ║\n"
            f"║          Hybrid Kernel — Linux · Windows · Kurono          ║\n"
            f"╚══════════════════════════════════════════════════════════════╝\n"
        )

    # ── Pipe support ──────────────────────────────────────────────────────────

    @staticmethod
    def parse_pipe_chain(command: str) -> List[Tuple[Optional[str], str]]:
        """Parse 'linux:ls | windows:findstr foo' into [(env, cmd), ...]"""
        segments = [s.strip() for s in command.split("|")]
        result = []
        for seg in segments:
            if ":" in seg and seg.split(":")[0] in ALL_ENVS:
                env, cmd = seg.split(":", 1)
                result.append((env.strip(), cmd.strip()))
            else:
                result.append((None, seg))
        return result
