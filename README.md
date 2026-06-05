# mycal

mycal is a lightweight terminal calendar and activity tracker. It stores your data in a standard SQLite database and renders directly in your terminal using standard ANSI escape sequences. No complex frameworks, no heavy dependencies—just a straightforward command-line tool designed to get completely out of your way.

## Getting Started

To build the project you just need the SQLite3 development headers installed on your system. Then simply run:
```
make
```

Once compiled, run the program to view your schedule for the day:
```
./mycal
```

Oops, your schedule is completely empty! Try this to add your very first activity:
```
./mycal --add-activity
```

mycal offers multiple view types and options. Try `./mycal --help` to discover more and explore the full list.

## Examples
To see a demonstration of the program's common usage, check this screencast: 

[mycal-video.webm](https://github.com/user-attachments/assets/f23fdbe1-2e20-4e0d-98b7-d780dff4ae13)

mycal can effectively display complex overlapping activities:

<img width="600" alt="overlapping-activities" src="https://github.com/user-attachments/assets/ce6f0f25-3667-4eb0-a1dd-745c509faefd" />

You can also use flexible and relative datetime strings to query your timeline.


```
#Load and inspect a custom database file in weekly view
./mycal work.db --show-week

# List the last 15 activities up to right now
./mycal --show-acts="now" 15

# View a specific calendar date 
./mycal --show-day="2026-05-06"

# See your agenda for tomorrow afternoon
./mycal -d"tomorrow 15:00"

# Look for the Friday of next week
./mycal -w"Friday +1w"
```

For full details on the datetime string formats, check out the built-in guide:
```
./mycal --help-datetime
```

_Now go plan some activities, drink good coffee, work hard, but don't stress too much!_
