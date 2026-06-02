# BestClient Graffity Server

TCP graffity placement server for BestClient.

Default public endpoint:

- `193.23.201.125:8781`

## Hardening

The server rejects or throttles abusive clients with:

- bounded line size
- hello-first handshake
- read/write deadlines
- max concurrent connections per IP
- per-IP connection/message/place/remove token buckets
- field length and coordinate validation

## Run

```bash
cd src/graffitysrv
./run.sh start
./run.sh status
./run.sh logs
```

Override bind/state path:

```bash
./run.sh start 0.0.0.0:8781 /root/run/graffitysrv/graffity_state.json
```
