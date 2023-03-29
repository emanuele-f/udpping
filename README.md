Simple ping with UDP

```
Usage: udpping [-s] [-c server] [-p port] [args]

Options:
  -s                   run as a server
  -c server            connect to the given server IP
  -p port              specify UDP port (default 6000)

Client options:
  -n packets           number of packets to send (default 4)
  -b size              size of the UDP payload (default 64 B)
  -i interval_ms       interval for the packets send (default 1000)
  -t timeout_ms        receiver thread socket timeout (default 100)
  -w wait_ms           timeout to wait for the reception of all the packets (default 500)
  -O seconds           omit the first n seconds from stats calculation
```
