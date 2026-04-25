from datetime import datetime, timezone
from flask_sqlalchemy import SQLAlchemy

db = SQLAlchemy()


class DoorEvent(db.Model):
    __tablename__ = "door_events"

    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(
        db.DateTime(timezone=True),
        nullable=False,
        default=lambda: datetime.now(timezone.utc),
    )
    action = db.Column(db.String(10), nullable=False)   # open, close, stall
    trigger = db.Column(db.String(20), default="unknown")  # schedule, manual, unknown
    run_duration_ms = db.Column(db.Integer)
    peak_current_ma = db.Column(db.Integer)
    stall_detected = db.Column(db.Boolean, default=False)
    action_completed = db.Column(db.Boolean, default=False)
    door_state = db.Column(db.String(10))               # open, closed, unknown
    firmware_version = db.Column(db.String(20))

    def __repr__(self):
        return f"<DoorEvent {self.id} {self.action} @ {self.timestamp}>"


class DeviceStatus(db.Model):
    __tablename__ = "device_status"

    id = db.Column(db.Integer, primary_key=True)
    last_poll_at = db.Column(db.DateTime(timezone=True))

    @classmethod
    def record_poll(cls):
        row = db.session.get(cls, 1)
        if row is None:
            row = cls(id=1, last_poll_at=datetime.now(timezone.utc))
            db.session.add(row)
        else:
            row.last_poll_at = datetime.now(timezone.utc)
        db.session.commit()

    @classmethod
    def get_last_poll(cls):
        row = db.session.get(cls, 1)
        return row.last_poll_at if row else None


class PendingCommand(db.Model):
    __tablename__ = "pending_commands"

    id = db.Column(db.Integer, primary_key=True)
    action = db.Column(db.String(10), nullable=False)   # open, close
    issued_at = db.Column(
        db.DateTime(timezone=True),
        nullable=False,
        default=lambda: datetime.now(timezone.utc),
    )
    expires_at = db.Column(db.DateTime(timezone=True), nullable=False)

    def is_expired(self):
        # SQLite returns naive datetimes; treat them as UTC
        expires = self.expires_at
        if expires.tzinfo is None:
            expires = expires.replace(tzinfo=timezone.utc)
        return datetime.now(timezone.utc) > expires
