from astral import LocationInfo
from astral.sun import sun
from datetime import date
from typing import Optional

CROWTHORNE = LocationInfo(
    name="Crowthorne",
    region="England",
    timezone="Europe/London",
    latitude=51.3644,
    longitude=-0.8,
)


def get_schedule(for_date: Optional[date] = None) -> tuple:
    """Return (open_at, close_at) as timezone-aware datetimes in Europe/London.

    open_at  = sunrise
    close_at = sunset

    Offsets can be added here later without touching ESP32 firmware.
    """
    if for_date is None:
        for_date = date.today()
    s = sun(CROWTHORNE.observer, date=for_date, tzinfo=CROWTHORNE.timezone)
    return s["sunrise"], s["sunset"]
