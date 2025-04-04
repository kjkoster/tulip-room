# Tulip Room Panel

This project uses [Lilygo T5](https://lilygo.cc/products/t5-4-7-inch-e-paper-v2-3)
panels as room panels to show the availability of rooms based on their Google
Calendars.

The calendar processing is handled by a server component. If you are looking for
a strictly client-only implementation, this project may not be for you.

## Server Setup

The back-end interfaces with the Google APIs to determine the room details and
its availability.

Having a server-side component is not strictly necessary, but there is no good,
headless Google API interface library for Arduino. Implementing the API manually
was less work than having a proxy in Python.

Deploy the room server as Docker container, giving it a public IP address.

```
docker build -t tulip-proxy .
docker save -o tulip-proxy.docker.tar tulip-proxy
```

Copy the TAR file over to the vm and there run:

```
docker load -i tulip-proxy.docker.tar
docker run -i -p 80:5000 -t tulip-proxy
```

If you get all sorts of read timeouts on the clients, check the CPU load of the
server. The calendar parser is really CPU hungry, so even for a single room
panel you need at least a `medium` instance on (for example) Google Cloud. On a
`small` instance takes too long for the calendar to load and things start timing
out. This is compounded by the fact that the calendar handling code is not
thread-safe.

## Programming the Panel

Programming the panel is tricky. It is very, very picky about your Arduino IDE
setup, core library versions and the hardware has a few issues to keep in mind
as well.

When installing, downgrade your core ESP libraries to a 2.x version. The demo
code used a few undocumented features of those libraries, so Lilygo locks you
into old libraries. The project in this repository was tested witj 2.0.17 of the
ESP core libraries.

**important:** Follow the instructions about setting up your tools config in the
Arduino IDE.  Failure to do so will result in all kinds of error messages.

We do not use partial updates for now, because alledgedly there is a hardware
issue that causes the display to become damaged with permanent ghosting. The
Lilygo-supplied driver library does not seem to be completely tested for partial
updates either. Better to optimise to keep screen updated minimal.

Finally, the board may randomly stop accepting updates. When that happens, use
some tweezers or a wire to pull `GPIO0` to `GND` while resetting the board. That
will put it into programming mode, and you can then start a new upload from the
Arduino IDE.

## Image Preparation

The images are compiled into the ESP32 image using byte arrays defined in header
files. Liligo kindly provided a Python script to take an image and generate the
file contents. The scripts are installed on your computer as part of the
`LilyGo-EPD47` library.

The commands below show how to run the scripts. Your system paths may be
different, so you may have to hunt around your file system to find the right
file path for `ARDUINO_LIB_DIR` and for `SRC_DIR`. These instructions have you
set up a Python virtual environment, so that the dependencies do not clash with
other Python projects you may have.

```bash
python3 -m venv venv
. venv/bin/activate
pip install pillow
python3 ${ARDUINO_LIB_DIR}/LilyGo-EPD47/scripts/imgconvert.py -i ${SRC_DIR}/battery-low-300x300.png -n battery_low -o ${SRC_DIR}/lilygo_47_room_panel/battery_low.h
```

You can use the recolour script to make an image grey. Specifically the free
room image should probably be grey to have good visual cue that the room is
available.

```
cat lilygo_47_room_panel/room_free.h | ./recolour-grey.sh > out
mv out lilygo_47_room_panel/room_free.h
```

