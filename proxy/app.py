import pytz
import time
import yaml
import datetime
import requests
import icalendar
import threading
import traceback
import polars as pl
from flask import Flask
import recurring_ical_events

COL_START, COL_END = "start", "end"
timezone = None
rooms = {}

def get_todays_events(url):
    response = requests.get(url)
    response.raise_for_status()

    now = datetime.datetime.now(pytz.utc).astimezone(timezone)
    end_of_day = datetime.datetime.combine(now.date(), datetime.time(23, 59, 59), tzinfo=timezone)

    a_calendar = icalendar.Calendar.from_ical(response.content)
    events = recurring_ical_events.of(a_calendar).between(now, end_of_day)

    records = []
    for event in events:
        start = event.get("DTSTART").dt.astimezone(timezone) if event.get("DTSTART") else None
        end = event.get("DTEND").dt.astimezone(timezone) if "DTEND" in event and event.get("DTEND") else None
        records.append((start, end))

    return now, pl.DataFrame(records, schema=[COL_START, COL_END], orient="row")


def merge_meetings(df):
    if df.is_empty():
        return df

    df = df.sort(COL_START)

    merged = []
    current_start, current_end = df[0, COL_START], df[0, COL_END]

    for row in df.iter_rows(named=True):
        if row[COL_START] <= current_end:  # Overlapping or consecutive meetings
            current_end = max(current_end, row[COL_END])
        else:
            merged.append({COL_START: current_start, COL_END: current_end})
            current_start, current_end = row[COL_START], row[COL_END]

    merged.append({COL_START: current_start, COL_END: current_end})

    return pl.DataFrame(merged, schema=[COL_START, COL_END])


def check_room_status(_df, _now):
    if _df.is_empty():
        return "free", "free for the day"

    active_meeting = _df.filter(
        (_df[COL_START] <= _now) & (_df[COL_END] > _now)
    )
    is_busy = not active_meeting.is_empty()

    if is_busy:
        next_change_time = _df.filter(_df[COL_END] > _now)[COL_END].min()
        return "busy", f"booked until {next_change_time:%H:%M}"
    else:
        next_meeting_start = _df.filter(_df[COL_START] > _now)[COL_START].min()
        next_change_time = (
            next_meeting_start if next_meeting_start is not None else None
        )
        if next_change_time is None:
            return "free", "free for the day"
        else:
            return "free", f"free until {next_change_time:%H:%M}"

# We update the availability in the background. If it is synchronouse, the ESP32
# has to wait for all the processing to complete. By putting the avalability
# determination into a daemon thread, all the web service does is return a
# string, which means the ESP32 can go back to sleep quicker.

availability = {}

def update_availability():
    while True:
        now = datetime.datetime.now(timezone)
        for mac_address, room in rooms.items():
            try:
                now, ical_df = get_todays_events(room['ical_url'])
                merged_ical_df = merge_meetings(ical_df)
                free_busy, message = check_room_status(merged_ical_df, now)
                availability[mac_address] = f"{room['name']}\n{free_busy}\n{message}\n"
            except Exception as e:
                traceback.print_exc()
                availability[mac_address] = f"{room['name']}\nerror\n{str(e)[:30]}\n"
        time.sleep(10)

app = Flask(__name__)

@app.route('/rooms/<mac_address>', methods=['GET'])
def get_room(mac_address):
    return availability.get(mac_address.upper(), "Unknown Room\nfree\nNo data available\n")

if __name__ == '__main__':
    with open('rooms.yml', 'r') as file:
        config = yaml.safe_load(file)


        timezone = pytz.timezone(config['timezone'])
        for room in config['rooms']:
            rooms.update({room['mac_address'].upper(): room})

    print(timezone)
    print(yaml.dump(rooms))

    threading.Thread(target=update_availability, daemon=True).start()
    app.run(host='0.0.0.0')
