from __future__ import annotations

import json
import os
from datetime import timedelta
from dataclasses import dataclass
from functools import wraps
from pathlib import Path
from typing import Any, Dict, List, Optional
from urllib.parse import quote, urlencode, urlparse

import requests
from flask import Flask, Response, abort, current_app, jsonify, redirect, render_template_string, request, session, url_for


APP_TEMPLATE = """
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>NeuralMimicry Webots</title>
    <style>
      :root {
        --bg: #f4efe8;
        --panel: #fffdf9;
        --ink: #172130;
        --muted: #5b6675;
        --line: #d8d1c5;
        --accent: #d96134;
        --accent-ink: #fff8f2;
        --shadow: 0 18px 40px rgba(23, 33, 48, 0.08);
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        font-family: "Segoe UI", system-ui, -apple-system, BlinkMacSystemFont, sans-serif;
        background: radial-gradient(circle at top, rgba(217, 97, 52, 0.14), transparent 40%), var(--bg);
        color: var(--ink);
      }
      a { color: inherit; }
      .shell {
        max-width: 1280px;
        margin: 0 auto;
        padding: 24px;
      }
      .hero {
        background: linear-gradient(135deg, rgba(217, 97, 52, 0.12), rgba(23, 33, 48, 0.04));
        border: 1px solid rgba(217, 97, 52, 0.18);
        border-radius: 28px;
        padding: 28px;
        box-shadow: var(--shadow);
      }
      .hero-top {
        display: flex;
        gap: 16px;
        justify-content: space-between;
        align-items: flex-start;
        flex-wrap: wrap;
      }
      .eyebrow {
        text-transform: uppercase;
        font-size: 12px;
        letter-spacing: 0.18em;
        color: var(--accent);
        margin: 0 0 10px;
      }
      h1 {
        margin: 0;
        font-size: clamp(2rem, 4vw, 3.5rem);
        line-height: 1.02;
      }
      .hero p {
        color: var(--muted);
        max-width: 860px;
        line-height: 1.55;
        margin: 16px 0 0;
      }
      .hero-actions {
        display: flex;
        gap: 12px;
        flex-wrap: wrap;
      }
      .button, button {
        appearance: none;
        border: 0;
        border-radius: 999px;
        cursor: pointer;
        padding: 12px 18px;
        font: inherit;
        background: var(--accent);
        color: var(--accent-ink);
        text-decoration: none;
      }
      .button.secondary, button.secondary {
        background: transparent;
        color: var(--ink);
        border: 1px solid var(--line);
      }
      .meta-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
        gap: 12px;
        margin-top: 22px;
      }
      .meta-card {
        background: rgba(255,255,255,0.76);
        border: 1px solid rgba(23, 33, 48, 0.08);
        border-radius: 20px;
        padding: 14px 16px;
      }
      .meta-card strong {
        display: block;
        font-size: 0.9rem;
      }
      .meta-card span {
        display: block;
        margin-top: 6px;
        color: var(--muted);
        font-size: 0.95rem;
      }
      .grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
        gap: 18px;
        margin-top: 24px;
      }
      .card {
        background: var(--panel);
        border: 1px solid rgba(23, 33, 48, 0.09);
        border-radius: 24px;
        padding: 22px;
        box-shadow: var(--shadow);
      }
      .card h2 {
        margin: 0;
        font-size: 1.35rem;
      }
      .summary {
        margin: 10px 0 0;
        color: var(--muted);
        line-height: 1.5;
      }
      .stats {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 10px;
        margin-top: 18px;
      }
      .stat {
        border: 1px solid var(--line);
        border-radius: 16px;
        padding: 10px 12px;
        background: #fff;
      }
      .stat strong {
        display: block;
        font-size: 1rem;
      }
      .stat span {
        display: block;
        color: var(--muted);
        margin-top: 4px;
        font-size: 0.86rem;
      }
      .chips {
        display: flex;
        gap: 8px;
        flex-wrap: wrap;
        margin-top: 14px;
      }
      .chip {
        border-radius: 999px;
        padding: 6px 10px;
        background: rgba(217, 97, 52, 0.12);
        color: var(--accent);
        font-size: 0.82rem;
      }
      .samples {
        margin-top: 18px;
      }
      .samples h3 {
        margin: 0 0 10px;
        font-size: 0.96rem;
      }
      .sample-list {
        list-style: none;
        margin: 0;
        padding: 0;
        display: grid;
        gap: 8px;
      }
      .sample-list li {
        border: 1px solid rgba(23, 33, 48, 0.07);
        border-radius: 14px;
        padding: 9px 10px;
        font-size: 0.88rem;
        color: var(--muted);
        background: rgba(255,255,255,0.86);
      }
      form {
        margin-top: 18px;
        display: grid;
        gap: 12px;
      }
      label {
        font-size: 0.88rem;
        color: var(--muted);
      }
      input {
        width: 100%;
        margin-top: 6px;
        padding: 11px 12px;
        border-radius: 14px;
        border: 1px solid var(--line);
        font: inherit;
        background: #fff;
      }
      .card-actions {
        display: flex;
        gap: 10px;
        flex-wrap: wrap;
        align-items: center;
      }
      .card-actions a {
        font-size: 0.88rem;
        color: var(--muted);
      }
      .notice {
        margin-top: 24px;
        padding: 18px 20px;
        border-radius: 20px;
        border: 1px solid rgba(23, 33, 48, 0.08);
        background: rgba(255, 255, 255, 0.7);
        color: var(--muted);
        line-height: 1.55;
      }
      @media (max-width: 640px) {
        .shell { padding: 16px; }
        .hero, .card { padding: 18px; border-radius: 22px; }
        .stats { grid-template-columns: 1fr; }
      }
    </style>
  </head>
  <body>
    <div class="shell">
      <section class="hero">
        <div class="hero-top">
          <div>
            <p class="eyebrow">Continuum Tenant Webots</p>
            <h1>Browser-only embodied worlds for authenticated AARNN users.</h1>
            <p>
              Open the persistent shared habitat. Eight AARNN robot slots share one evolving world with wildlife NPCs;
              the simulation continues running when every browser disconnects.
            </p>
          </div>
          <div class="hero-actions">
            {% if aarnn_app_base %}
            <a class="button secondary" href="{{ aarnn_app_base }}">Open AARNN</a>
            {% endif %}
            <a class="button secondary" href="{{ url_for('api_catalog') }}">Catalog API</a>
            <a class="button secondary" href="{{ url_for('logout') }}">Logout</a>
          </div>
        </div>
        <div class="meta-grid">
          <div class="meta-card">
            <strong>User</strong>
            <span>{{ session_identity.user }}</span>
          </div>
          <div class="meta-card">
            <strong>Role</strong>
            <span>{{ session_identity.role or 'user' }}</span>
          </div>
          <div class="meta-card">
            <strong>Groups</strong>
            <span>{{ session_identity.groups|join(', ') if session_identity.groups else 'user' }}</span>
          </div>
          <div class="meta-card">
            <strong>Active Team</strong>
            <span>{{ session_identity.active_team or 'individual workspace' }}</span>
          </div>
        </div>
      </section>

      <div class="grid">
        {% for world in worlds %}
        <article class="card">
          <h2>{{ world.title }}</h2>
          <p class="summary">{{ world.summary }}</p>
          <div class="stats">
            <div class="stat">
              <strong>{{ world.sensory_count }}</strong>
              <span>Sensory channels</span>
            </div>
            <div class="stat">
              <strong>{{ world.output_count }}</strong>
              <span>Actuator channels</span>
            </div>
            <div class="stat">
              <strong>{{ world.hidden_layers }}</strong>
              <span>Hidden layers</span>
            </div>
            <div class="stat">
              <strong>{{ world.aarnn_profile or 'custom' }}</strong>
              <span>AARNN profile</span>
            </div>
          </div>

          {% if world.aliases %}
          <div class="chips">
            {% for alias in world.aliases %}
            <span class="chip">{{ alias }}</span>
            {% endfor %}
          </div>
          {% endif %}

          <div class="samples">
            <h3>Sample sensory ports</h3>
            <ul class="sample-list">
              {% for item in world.sample_inputs %}
              <li>{{ item }}</li>
              {% endfor %}
              {% if not world.sample_inputs %}
              <li>No explicit sensory alignment file was supplied for this world.</li>
              {% endif %}
            </ul>
          </div>

          <div class="samples">
            <h3>Sample actuator ports</h3>
            <ul class="sample-list">
              {% for item in world.sample_outputs %}
              <li>{{ item }}</li>
              {% endfor %}
              {% if not world.sample_outputs %}
              <li>No explicit actuator alignment file was supplied for this world.</li>
              {% endif %}
            </ul>
          </div>

          <form action="{{ url_for('launch_world', world_id=world.id) }}" method="get">
            <label>
              Optional AARNN workspace label
              <input type="text" name="workspace" placeholder="{{ world.workspace_hint or 'workspace-name' }}">
            </label>
            <div class="card-actions">
              {% if world.shared_world or world.launch_url %}
              <button type="submit">Open Persistent Shared World</button>
              {% else %}
              <button type="button" class="secondary" disabled>Launch not configured</button>
              {% endif %}
              {% if world.source_world_url %}
              <a href="{{ world.source_world_url }}">World source</a>
              {% endif %}
            </div>
          </form>
        </article>
        {% endfor %}
      </div>

      <div class="notice">
        {% if aarnn_app_base %}
        Use the same customer identity on the AARNN service and this Webots service. The launch cards here expose the generated
        I/O alignment metadata so users can bind their AARNN workspace synapses to the selected world profile without guessing the
        sensor or actuator ordering.
        {% else %}
        Set <code>WEBOTS_AARNN_APP_BASE</code> to surface a direct AARNN handoff alongside the world catalog.
        {% endif %}
      </div>
    </div>
  </body>
</html>
"""

LANDING_TEMPLATE = """
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>NeuralMimicry Webots</title>
    <style>
      body {
        margin: 0;
        font-family: "Segoe UI", system-ui, -apple-system, BlinkMacSystemFont, sans-serif;
        background: linear-gradient(160deg, #0f1826 0%, #1d2b39 60%, #efede7 60%, #f6f2ea 100%);
        color: #172130;
      }
      .shell {
        max-width: 880px;
        margin: 0 auto;
        padding: 24px;
      }
      .card {
        margin-top: 10vh;
        background: rgba(255, 252, 247, 0.96);
        border-radius: 28px;
        padding: 30px;
        box-shadow: 0 24px 60px rgba(0, 0, 0, 0.18);
      }
      p:first-child {
        text-transform: uppercase;
        letter-spacing: 0.18em;
        color: #d96134;
        font-size: 12px;
        margin: 0 0 12px;
      }
      h1 {
        margin: 0;
        font-size: clamp(2rem, 5vw, 3.5rem);
        line-height: 1.04;
      }
      .copy {
        color: #5b6675;
        line-height: 1.65;
        margin-top: 16px;
      }
      .actions {
        display: flex;
        flex-wrap: wrap;
        gap: 12px;
        margin-top: 22px;
      }
      a {
        text-decoration: none;
        border-radius: 999px;
        padding: 12px 18px;
        background: #d96134;
        color: #fff7ef;
      }
      a.secondary {
        background: transparent;
        color: #172130;
        border: 1px solid #d9d2c6;
      }
      code {
        background: rgba(23, 33, 48, 0.08);
        border-radius: 8px;
        padding: 2px 6px;
      }
    </style>
  </head>
  <body>
    <div class="shell">
      <section class="card">
        <p>Continuum Tenant Webots</p>
        <h1>Authenticated browser launch for Webots embodiments.</h1>
        <div class="copy">
          <p>This service expects a centralized access-token exchange from the commercial NeuralMimicry site or another trusted launcher.</p>
          <p>Post an access token to <code>/auth/access/exchange</code> with an optional <code>next</code> path, or open the commercial site launch page and start from there.</p>
        </div>
        <div class="actions">
          {% if site_base_url %}
          <a href="{{ site_base_url }}/webots">Open website launch page</a>
          {% endif %}
          <a class="secondary" href="{{ url_for('api_health') }}">Health</a>
        </div>
      </section>
    </div>
  </body>
</html>
"""

STREAM_VIEWER_TEMPLATE = """
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>NeuralMimicry Shared Webots World</title>
    <style>
      html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; background: #101820; color: #f4f1e9; font: 15px system-ui, sans-serif; }
      .bar { height: 52px; display: flex; align-items: center; gap: 18px; padding: 0 18px; background: #172130; }
      .bar strong { letter-spacing: .08em; text-transform: uppercase; font-size: 12px; }
      .bar a { color: #f2a078; margin-left: auto; }
      #status { color: #a9d5b0; }
      webots-view { display: block; width: 100%; height: calc(100% - 52px); }
      webots-view canvas { width: 100%; height: 100%; }
    </style>
    <script type="module" src="https://cyberbotics.com/wwi/R2025a/WebotsView.js"></script>
  </head>
  <body>
    <header class="bar">
      <strong>NeuralMimicry · Shared Ecology</strong>
      <span id="status">Connecting to the persistent world…</span>
      <a href="{{ site_base_url }}/webots">Back to NeuralMimicry</a>
    </header>
    <webots-view></webots-view>
    <script type="module">
      await customElements.whenDefined('webots-view');
      const viewer = document.querySelector('webots-view');
      const status = document.getElementById('status');
      const streamUrl = {{ stream_url | tojson }};
      const mobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);
      viewer.showWorldSelection = false;
      viewer.showIde = false;
      viewer.onready = () => { status.textContent = 'Connected · shared simulation continues when you leave'; };
      viewer.ondisconnect = () => { status.textContent = 'Disconnected · the shared simulation is still running'; };
      viewer.connect(streamUrl, 'w3d', false, mobile, -1);
    </script>
  </body>
</html>
"""


@dataclass(frozen=True)
class Settings:
    host: str
    port: int
    secret_key: str
    session_cookie_name: str
    cookie_domain: str
    secure_cookies: bool
    site_base_url: str
    aarnn_app_base: str
    central_auth_api_base: str
    central_auth_timeout_secs: float
    catalog_path: Path
    world_source_base_url: str
    cloud_run_base_url: str
    stream_url: str
    default_world: str
    launch_overrides: Dict[str, str]

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(
            host=os.getenv("WEBOTS_HOST", "0.0.0.0").strip() or "0.0.0.0",
            port=int(os.getenv("WEBOTS_PORT", "8080") or "8080"),
            secret_key=(os.getenv("WEBOTS_SECRET_KEY", "webots-dev-secret-change-me") or "webots-dev-secret-change-me").strip(),
            session_cookie_name=(os.getenv("WEBOTS_SESSION_COOKIE_NAME", "nm_webots_session") or "nm_webots_session").strip(),
            cookie_domain=(os.getenv("WEBOTS_COOKIE_DOMAIN", "") or "").strip(),
            secure_cookies=_env_bool("WEBOTS_SECURE_COOKIES", True),
            site_base_url=(os.getenv("WEBOTS_SITE_BASE_URL", "https://neuralmimicry.ai") or "").strip().rstrip("/"),
            aarnn_app_base=(os.getenv("WEBOTS_AARNN_APP_BASE", "https://aarnn.neuralmimicry.ai") or "").strip().rstrip("/"),
            central_auth_api_base=(os.getenv("WEBOTS_CENTRAL_AUTH_API_BASE", "https://api.neuralmimicry.ai") or "").strip().rstrip("/"),
            central_auth_timeout_secs=float(os.getenv("WEBOTS_CENTRAL_AUTH_TIMEOUT_SECS", "10") or "10"),
            catalog_path=Path(os.getenv("WEBOTS_CATALOG_PATH", Path(__file__).with_name("default_catalog.json"))),
            world_source_base_url=(os.getenv("WEBOTS_WORLD_SOURCE_BASE_URL", "") or "").strip().rstrip("/"),
            cloud_run_base_url=(os.getenv("WEBOTS_CLOUD_RUN_BASE_URL", "") or "").strip().rstrip("/"),
            stream_url=(os.getenv("WEBOTS_STREAM_URL", "wss://webots.neuralmimicry.ai/stream") or "").strip(),
            default_world=(os.getenv("WEBOTS_DEFAULT_WORLD", "shared-fleet") or "shared-fleet").strip(),
            launch_overrides=_load_json_mapping(os.getenv("WEBOTS_LAUNCH_OVERRIDES_JSON", "{}")),
        )


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    value = raw.strip().lower()
    if not value:
        return default
    return value in {"1", "true", "yes", "y", "on"}


def _load_json_mapping(raw: str) -> Dict[str, str]:
    try:
        data = json.loads(raw or "{}")
    except json.JSONDecodeError:
        return {}
    if not isinstance(data, dict):
        return {}
    result: Dict[str, str] = {}
    for key, value in data.items():
        key_s = str(key).strip()
        value_s = str(value).strip()
        if key_s and value_s:
            result[key_s] = value_s
    return result


def _read_json(path: Optional[Path]) -> Dict[str, Any]:
    if path is None or not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _safe_next_path(value: Optional[str]) -> str:
    raw = (value or "").strip()
    if not raw:
        return "/app"
    if raw.startswith("//"):
        return "/app"
    parsed = urlparse(raw)
    if parsed.scheme or parsed.netloc:
        return "/app"
    return raw if raw.startswith("/") else f"/{raw}"


def _source_world_url(base_url: str, world_path: str) -> str:
    base = (base_url or "").strip().rstrip("/")
    relative = str(world_path or "").strip().lstrip("/")
    if not base or not relative:
        return ""
    return f"{base}/{relative}"


def _cloud_launch_url(run_base_url: str, source_world_url: str) -> str:
    base = (run_base_url or "").strip().rstrip("/")
    if not base or not source_world_url:
        return ""
    return f"{base}?url={quote(source_world_url, safe='')}"


def _session_identity() -> Dict[str, Any]:
    return {
        "authenticated": bool(session.get("user") and session.get("webots_use")),
        "user": session.get("user"),
        "role": session.get("role"),
        "groups": session.get("groups") or [],
        "active_team": session.get("active_team"),
        "service_access": session.get("service_access") or {},
    }


def _identity_can_use_webots(identity: Dict[str, Any]) -> bool:
    role = str(identity.get("role") or "").strip().lower()
    groups = {str(item).strip().lower() for item in (identity.get("groups") or [])}
    if role == "admin" or "admin" in groups:
        return True

    access = identity.get("service_access") or {}
    if isinstance(access, list):
        entry = next(
            (item for item in access if isinstance(item, dict) and str(item.get("service_key") or item.get("key") or "").strip().lower() == "webots"),
            {},
        )
    elif isinstance(access, dict):
        entry = access.get("webots") or {}
    else:
        entry = {}
    if isinstance(entry, str):
        return entry.strip().lower() in {"use", "control"}
    if not isinstance(entry, dict):
        return False
    level = str(entry.get("access_level") or entry.get("level") or "").strip().lower()
    return bool(entry.get("can_use") or entry.get("can_control")) or level in {"use", "control"}


def _trusted_exchange_origin(
    origin: str,
    site_base_url: str,
    request_host: str,
    require_https: bool = True,
) -> bool:
    parsed = urlparse((origin or "").strip())
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        return False
    if require_https and parsed.scheme != "https":
        return False
    expected_port = 443 if parsed.scheme == "https" else 80
    if parsed.port is not None and parsed.port != expected_port:
        return False
    trusted_hosts = {
        value
        for value in (
            urlparse(site_base_url).hostname,
            (request_host or "").split(":", 1)[0],
        )
        if value
    }
    return parsed.hostname.lower() in {value.lower() for value in trusted_hosts}


def _access_exchange_failure(error: str, status: int, details: str = "") -> Response:
    payload: Dict[str, Any] = {"error": error}
    if details and request.is_json:
        payload["details"] = details
    response = jsonify(payload)
    response.status_code = status

    origin = request.headers.get("Origin", "").strip()
    is_browser_form = not request.is_json and request.accept_mimetypes.accept_html
    if is_browser_form and origin and _trusted_exchange_origin(
        origin,
        _settings().site_base_url,
        request.host,
        _settings().secure_cookies,
    ):
        return redirect(
            f"{_settings().site_base_url}/webots?{urlencode({'webots_error': error})}",
            code=303,
        )
    return response


def _catalog() -> Dict[str, Dict[str, Any]]:
    return current_app.extensions["webots_catalog"]


def _settings() -> Settings:
    return current_app.extensions["webots_settings"]


def _resolve_world(world_id: str) -> Optional[Dict[str, Any]]:
    lookup = (world_id or "").strip().lower()
    if not lookup:
        return None
    for world in _catalog().values():
        aliases = {world["id"], *(alias.lower() for alias in world.get("aliases", []))}
        if lookup in aliases:
            return world
    return None


def _login_required(view):
    @wraps(view)
    def wrapped(*args: Any, **kwargs: Any):
        if session.get("user") and session.get("webots_use"):
            return view(*args, **kwargs)
        wants_json = request.path.startswith("/api/") or "application/json" in (request.headers.get("Accept") or "")
        if wants_json:
            return jsonify({"authenticated": False, "error": "unauthorized"}), 401
        return redirect(url_for("landing"))

    return wrapped


def _load_catalog(settings: Settings) -> Dict[str, Dict[str, Any]]:
    raw_items = []
    try:
        raw = settings.catalog_path.read_text(encoding="utf-8")
        raw_items = json.loads(raw)
    except (OSError, json.JSONDecodeError):
        raw_items = []
    if not isinstance(raw_items, list):
        raw_items = []

    repo_root = settings.catalog_path.parent.parent if settings.catalog_path.name == "default_catalog.json" else Path("/app")
    catalog: Dict[str, Dict[str, Any]] = {}
    for item in raw_items:
        if not isinstance(item, dict):
            continue
        world_id = str(item.get("id") or "").strip()
        if not world_id:
            continue
        world_path = str(item.get("world_path") or "").strip()
        config_path = str(item.get("config_path") or "").strip()
        io_alignment_path = str(item.get("io_alignment_path") or "").strip()
        config_json = _read_json(repo_root / config_path) if config_path else {}
        alignment_json = _read_json(repo_root / io_alignment_path) if io_alignment_path else {}
        sensory = alignment_json.get("sensory_channels") or []
        outputs = alignment_json.get("output_channels") or []
        source_world_url = str(item.get("source_world_url") or "").strip() or _source_world_url(settings.world_source_base_url, world_path)
        launch_url = (
            str(item.get("launch_url") or "").strip()
            or settings.launch_overrides.get(world_id, "")
            or _cloud_launch_url(settings.cloud_run_base_url, source_world_url)
        )
        catalog[world_id] = {
            "id": world_id,
            "title": str(item.get("title") or world_id),
            "summary": str(item.get("summary") or "").strip(),
            "description": str(item.get("description") or "").strip(),
            "aliases": [str(alias).strip() for alias in (item.get("aliases") or []) if str(alias).strip()],
            "world_path": world_path,
            "config_path": config_path,
            "io_alignment_path": io_alignment_path,
            "workspace_hint": str(item.get("workspace_hint") or "").strip(),
            "aarnn_profile": str(((config_json.get("spike_io") or {}).get("profile") or item.get("aarnn_profile") or "")).strip(),
            "sensory_count": int(config_json.get("num_sensory_neurons") or len(sensory) or 0),
            "output_count": int(config_json.get("num_output_neurons") or len(outputs) or 0),
            "hidden_layers": int(config_json.get("num_hidden_layers") or 0),
            "source_world_url": source_world_url,
            "launch_url": launch_url,
            "shared_world": bool(item.get("shared_world", False)),
            "sample_inputs": [
                str(channel.get("device_port") or channel.get("connectome_node_id") or "").strip()
                for channel in sensory[:6]
                if str(channel.get("device_port") or channel.get("connectome_node_id") or "").strip()
            ],
            "sample_outputs": [
                str(channel.get("actuator_name") or channel.get("connectome_node_id") or "").strip()
                for channel in outputs[:6]
                if str(channel.get("actuator_name") or channel.get("connectome_node_id") or "").strip()
            ],
        }
    return catalog


def _verify_access_token(access_token: str, cookie_header: str = "") -> Dict[str, Any]:
    base = _settings().central_auth_api_base
    if not base:
        raise requests.RequestException("central auth API is not configured")
    headers = {"Accept": "application/json"}
    if access_token:
        headers["Authorization"] = f"Bearer {access_token}"
    if cookie_header:
        # Customers shares its browser-session cookie across neuralmimicry.ai
        # subdomains. Forward it only to the configured internal auth API so a
        # valid site session can complete the handoff after a cached bearer
        # token has expired or been cleared.
        headers["Cookie"] = cookie_header

    def fetch_session(request_headers: Dict[str, str]) -> requests.Response:
        response = requests.get(
            f"{base}/api/session",
            headers=request_headers,
            timeout=_settings().central_auth_timeout_secs,
        )
        if response.status_code in (408, 425, 429) or response.status_code >= 500:
            raise requests.HTTPError(
                f"central auth returned HTTP {response.status_code}", response=response
            )
        return response

    response = fetch_session(headers)
    if response.status_code == 401 and access_token and cookie_header:
        # An expired cached bearer must not hide a still-valid shared browser
        # session. Retry that case with the cookie alone; do not retry backend
        # failures as if they were credential rejection.
        cookie_only_headers = {
            key: value for key, value in headers.items() if key != "Authorization"
        }
        response = fetch_session(cookie_only_headers)
    if response.status_code >= 400:
        return {"authenticated": False}
    try:
        data = response.json()
    except ValueError as error:
        raise requests.RequestException("central auth returned invalid JSON") from error
    if not isinstance(data, dict):
        raise requests.RequestException("central auth returned an invalid session payload")
    return data


def create_app(settings: Optional[Settings] = None) -> Flask:
    settings = settings or Settings.from_env()
    app = Flask(__name__)
    app.config.update(
        SECRET_KEY=settings.secret_key,
        SESSION_COOKIE_NAME=settings.session_cookie_name,
        SESSION_COOKIE_DOMAIN=settings.cookie_domain or None,
        SESSION_COOKIE_SAMESITE="Lax",
        SESSION_COOKIE_SECURE=settings.secure_cookies,
        SESSION_COOKIE_HTTPONLY=True,
        PERMANENT_SESSION_LIFETIME=timedelta(hours=12),
    )
    app.extensions["webots_settings"] = settings
    app.extensions["webots_catalog"] = _load_catalog(settings)

    @app.route("/")
    def landing() -> Response:
        if session.get("user"):
            return redirect(url_for("app_page"))
        return render_template_string(LANDING_TEMPLATE, site_base_url=settings.site_base_url)

    @app.route("/api/health")
    def api_health() -> Response:
        return jsonify(
            {
                "service": "webots",
                "status": "ok",
                "worlds": len(_catalog()),
                "default_world": settings.default_world,
            }
        )

    @app.route("/api/session")
    def api_session() -> Response:
        return jsonify(_session_identity())

    @app.route("/api/stream/authorize")
    @_login_required
    def api_stream_authorize() -> Response:
        return jsonify({"authorized": True})

    @app.route("/api/catalog")
    @_login_required
    def api_catalog() -> Response:
        return jsonify(
            {
                "authenticated": True,
                "default_world": settings.default_world,
                "aarnn_app_base": settings.aarnn_app_base,
                "worlds": list(_catalog().values()),
            }
        )

    @app.route("/api/catalog/<world_id>")
    @_login_required
    def api_catalog_world(world_id: str) -> Response:
        world = _resolve_world(world_id)
        if not world:
            abort(404)
        return jsonify(world)

    @app.route("/api/launch/<world_id>")
    @_login_required
    def api_launch(world_id: str) -> Response:
        world = _resolve_world(world_id)
        if not world:
            abort(404)
        return jsonify(
            {
                "authenticated": True,
                "world": world,
                "workspace": (request.args.get("workspace") or "").strip(),
                "redirect_url": url_for("viewer_page") if world.get("shared_world") else (world.get("launch_url") or ""),
            }
        )

    @app.route("/app")
    @_login_required
    def app_page() -> Response:
        worlds = list(_catalog().values())
        worlds.sort(key=lambda item: item.get("title") or item.get("id"))
        return render_template_string(
            APP_TEMPLATE,
            worlds=worlds,
            session_identity=_session_identity(),
            aarnn_app_base=settings.aarnn_app_base,
        )

    @app.route("/viewer")
    @_login_required
    def viewer_page() -> Response:
        return render_template_string(
            STREAM_VIEWER_TEMPLATE,
            site_base_url=settings.site_base_url,
            stream_url=settings.stream_url,
        )

    @app.route("/auth/access/exchange", methods=["POST"])
    def access_exchange() -> Response:
        access_token = ""
        if request.is_json:
            payload = request.get_json(silent=True) or {}
            access_token = str(payload.get("access_token") or "").strip()
            next_path = _safe_next_path(payload.get("next"))
        else:
            access_token = str(request.form.get("access_token") or request.values.get("access_token") or "").strip()
            next_path = _safe_next_path(request.form.get("next") or request.values.get("next"))

        cookie_header = request.headers.get("Cookie", "").strip()
        origin = request.headers.get("Origin", "").strip()
        if not access_token and not cookie_header:
            return _access_exchange_failure("unauthorized", 401)
        if (cookie_header or origin) and not _trusted_exchange_origin(
            origin,
            settings.site_base_url,
            request.host,
            settings.secure_cookies,
        ):
            return _access_exchange_failure("untrusted_exchange_origin", 403)

        try:
            identity = _verify_access_token(access_token, cookie_header)
        except requests.RequestException as exc:
            return _access_exchange_failure("auth_unavailable", 502, str(exc))

        if not identity.get("authenticated"):
            return _access_exchange_failure("unauthorized", 401)

        if not _identity_can_use_webots(identity):
            return _access_exchange_failure("webots_use_access_required", 403)

        session["user"] = str(identity.get("user") or "").strip()
        session["role"] = str(identity.get("role") or "").strip() or None
        session["groups"] = [str(item).strip() for item in (identity.get("groups") or []) if str(item).strip()]
        active_team = identity.get("active_team")
        if isinstance(active_team, dict):
            session["active_team"] = str(active_team.get("name") or active_team.get("team_id") or "").strip() or None
        else:
            session["active_team"] = str(active_team or "").strip() or None
        session["service_access"] = identity.get("service_access") or {}
        session["webots_use"] = True
        session.permanent = True

        if request.is_json:
            return jsonify({"status": "ok", "next": next_path, **_session_identity()})
        return redirect(next_path, code=303)

    @app.route("/auth/logout")
    def logout() -> Response:
        session.clear()
        next_path = _safe_next_path(request.args.get("next") or "/")
        return redirect(next_path)

    @app.route("/launch/<world_id>")
    @_login_required
    def launch_world(world_id: str) -> Response:
        world = _resolve_world(world_id)
        if not world:
            abort(404)
        workspace = (request.args.get("workspace") or "").strip()
        session["last_launch"] = {
            "world": world["id"],
            "workspace": workspace,
        }
        if world.get("shared_world"):
            return redirect(url_for("viewer_page"))
        launch_url = str(world.get("launch_url") or "").strip()
        if not launch_url:
            return jsonify({"error": "launch_not_configured", "world": world_id}), 503
        return redirect(launch_url)

    return app
