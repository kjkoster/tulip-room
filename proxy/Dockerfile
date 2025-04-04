FROM python:3.13-slim

RUN pip install flask requests polars pytz icalendar recurring-ical-events pyyaml

WORKDIR /app
COPY app.py /app/
COPY rooms.yml /app/

CMD ["python", "app.py"]
