import os
from zoneinfo import ZoneInfo

LONDON_TZ = ZoneInfo("Europe/London")


class Config:
    SECRET_KEY = os.environ.get("SECRET_KEY", "dev-secret-change-me")
    API_KEY = os.environ.get("API_KEY", "")
    ADMIN_PASSWORD = os.environ.get("ADMIN_PASSWORD", "")
    SQLALCHEMY_TRACK_MODIFICATIONS = False


class DevelopmentConfig(Config):
    DEBUG = True
    SQLALCHEMY_DATABASE_URI = "sqlite:///coop.db"


class ProductionConfig(Config):
    DEBUG = False
    _db_url = os.environ.get("DATABASE_URL", "")
    # Railway injects postgres:// but SQLAlchemy 1.4+ requires postgresql://
    SQLALCHEMY_DATABASE_URI = _db_url.replace("postgres://", "postgresql://", 1)
    SQLALCHEMY_ENGINE_OPTIONS = {"pool_pre_ping": True}


config = {
    "development": DevelopmentConfig,
    "production": ProductionConfig,
}
