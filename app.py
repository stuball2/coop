import os
from datetime import datetime, timezone, timedelta
from functools import wraps
from zoneinfo import ZoneInfo

from dotenv import load_dotenv
load_dotenv()

from flask import Flask, request, jsonify, render_template, redirect, url_for, flash, session

from config import config, LONDON_TZ
from models import db, DoorEvent, PendingCommand
from notify import send_telegram
from sun_schedule import get_schedule


def create_app():
    app = Flask(__name__)
    env = os.environ.get("FLASK_ENV", "development")
    app.config.from_object(config[env])
    db.init_app(app)

    with app.app_context():
        db.create_all()

    # ------------------------------------------------------------------
    # Auth helpers
    # ------------------------------------------------------------------

    def require_api_key(f):
        @wraps(f)
        def decorated(*args, **kwargs):
            key = request.headers.get("X-API-Key", "")
            if not app.config["API_KEY"] or key != app.config["API_KEY"]:
                return jsonify({"error": "unauthorized"}), 401
            return f(*args, **kwargs)
        return decorated

    def check_admin_password(password: str) -> bool:
        admin = app.config.get("ADMIN_PASSWORD", "")
        return bool(admin) and password == admin

    def login_required(f):
        @wraps(f)
        def decorated(*args, **kwargs):
            if not session.get("logged_in"):
                return redirect(url_for("login"))
            return f(*args, **kwargs)
        return decorated

    # ------------------------------------------------------------------
    # Login / logout
    # ------------------------------------------------------------------

    @app.before_request
    def redirect_to_www():
        host = request.host.split(":")[0]
        if host == "www.broadmoorcoop.co.uk":
            url = request.url.replace("://www.broadmoorcoop.co.uk", "://broadmoorcoop.co.uk", 1)
            return redirect(url, 301)

    @app.route("/login", methods=["GET", "POST"])
    def login():
        if request.method == "POST":
            if check_admin_password(request.form.get("password", "")):
                session["logged_in"] = True
                return redirect(url_for("dashboard"))
            flash("Wrong password.")
        return render_template("login.html")

    @app.route("/logout")
    def logout():
        session.clear()
        return redirect(url_for("login"))

    # ------------------------------------------------------------------
    # Template helpers
    # ------------------------------------------------------------------

    @app.template_filter("local_dt")
    def local_dt(dt):
        """Format a datetime in Europe/London time for display."""
        if dt is None:
            return "—"
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.astimezone(LONDON_TZ).strftime("%d %b %H:%M:%S")

    @app.template_filter("local_time")
    def local_time(dt):
        if dt is None:
            return "—"
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.astimezone(LONDON_TZ).strftime("%H:%M")

    # ------------------------------------------------------------------
    # Dashboard
    # ------------------------------------------------------------------

    @app.route("/")
    @login_required
    def dashboard():
        open_at, close_at = get_schedule()
        events = DoorEvent.query.order_by(DoorEvent.timestamp.desc()).limit(20).all()
        last_event = events[0] if events else None
        door_state = last_event.door_state if last_event else "unknown"

        pending = PendingCommand.query.first()
        if pending and pending.is_expired():
            db.session.delete(pending)
            db.session.commit()
            pending = None

        now_local = datetime.now(LONDON_TZ)

        return render_template(
            "index.html",
            door_state=door_state,
            open_at=open_at,
            close_at=close_at,
            events=events,
            pending=pending,
            last_event=last_event,
            now=now_local,
        )

    # ------------------------------------------------------------------
    # ESP32 API
    # ------------------------------------------------------------------

    @app.route("/api/schedule")
    @require_api_key
    def api_schedule():
        open_at, close_at = get_schedule()

        pending = PendingCommand.query.first()
        if pending and pending.is_expired():
            db.session.delete(pending)
            db.session.commit()
            pending = None

        last_event = DoorEvent.query.order_by(DoorEvent.timestamp.desc()).first()
        door_state = last_event.door_state if last_event else "unknown"

        return jsonify({
            "sunrise": open_at.isoformat(),
            "sunset": close_at.isoformat(),
            "open_at": open_at.isoformat(),
            "close_at": close_at.isoformat(),
            # Unix epoch seconds (UTC) — easier for ESP32 to compare against time()
            "open_ts": int(open_at.timestamp()),
            "close_ts": int(close_at.timestamp()),
            "server_time": datetime.now(LONDON_TZ).isoformat(),
            "pending_command": pending.action if pending else None,
            "door_state": door_state,
        })

    @app.route("/api/telemetry", methods=["POST"])
    @require_api_key
    def api_telemetry():
        data = request.get_json(silent=True) or {}

        action = data.get("action")
        if not action:
            return jsonify({"error": "action required"}), 400

        door_state = data.get("door_state", "unknown")
        stall = data.get("stall_detected", False)
        completed = data.get("action_completed", False)
        duration_ms = data.get("run_duration_ms")
        peak_ma = data.get("peak_current_ma")

        event = DoorEvent(
            action=action,
            trigger=data.get("trigger", "unknown"),
            run_duration_ms=duration_ms,
            peak_current_ma=peak_ma,
            stall_detected=stall,
            action_completed=completed,
            door_state=door_state,
            firmware_version=data.get("firmware_version"),
        )
        db.session.add(event)

        if completed:
            PendingCommand.query.delete()

        db.session.commit()

        # Build Telegram notification
        parts = [f"Coop door {action.upper()}"]
        if stall:
            parts.append("(stall detected)")
        if duration_ms:
            parts.append(f"{duration_ms}ms")
        if peak_ma:
            parts.append(f"peak {peak_ma}mA")
        send_telegram(" — ".join(parts[:1]) + (" " + ", ".join(parts[1:]) if len(parts) > 1 else ""))

        return jsonify({"ok": True})

    # ------------------------------------------------------------------
    # Command endpoint — used by both dashboard (form POST) and API clients
    # ------------------------------------------------------------------

    @app.route("/api/command", methods=["POST"])
    def api_command():
        # Determine auth method
        if request.is_json:
            data = request.get_json()
            action = data.get("action")
            api_key = request.headers.get("X-API-Key", "")
            admin_pw = data.get("admin_password", "")
            authed = (api_key == app.config.get("API_KEY", "")) or check_admin_password(admin_pw)
        else:
            action = request.form.get("action")
            authed = session.get("logged_in", False)

        if not authed:
            if request.is_json:
                return jsonify({"error": "unauthorized"}), 401
            flash("Wrong password.")
            return redirect(url_for("dashboard"))

        if action not in ("open", "close"):
            if request.is_json:
                return jsonify({"error": "action must be 'open' or 'close'"}), 400
            flash("Invalid action.")
            return redirect(url_for("dashboard"))

        # One pending command at a time
        PendingCommand.query.delete()
        pending = PendingCommand(
            action=action,
            expires_at=datetime.now(timezone.utc) + timedelta(minutes=10),
        )
        db.session.add(pending)
        db.session.commit()

        if request.is_json:
            return jsonify({"ok": True, "pending_command": action})
        flash(f"Command queued: {action.upper()}")
        return redirect(url_for("dashboard"))

    return app


app = create_app()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 5000)))
