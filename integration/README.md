# Convex C++ Client — Integration Test Environment

This directory hosts a self-contained Convex environment used by the C++ Convex
client integration tests. It provides:

- A **local self-hosted** open-source Convex backend running in Docker.
- A minimal **Convex TypeScript project** (`convex-test-project/`) whose schema
  and functions exercise the full Convex value space.
- The project deployed to **both** the local backend and the shared **cloud dev**
  deployment, so tests can run against either target.

```
integration/
├── backend/
│   └── docker-compose.yml      # self-hosted convex-backend (+ optional dashboard)
├── convex-test-project/        # the Convex project (schema + functions)
│   ├── package.json
│   ├── convex/
│   │   ├── schema.ts
│   │   ├── messages.ts         # list / send / clearAll
│   │   ├── counters.ts         # get / increment
│   │   ├── values.ts           # echoQuery / echoMutation / kitchenSink
│   │   ├── errors.ts           # throwConvexError / throwPlainError
│   │   └── actions.ts          # echoAction / now
│   └── .env.local              # auto-written by `convex dev` (cloud creds; gitignored)
├── local.env                   # local backend URL + admin key (SECRET, gitignore)
└── README.md                   # this file
```

## Ports

| Port | Purpose                                             |
|------|-----------------------------------------------------|
| 3210 | Convex API (client protocol: `/version`, `/api/query`, `/api/mutation`, ...) |
| 3211 | Site / HTTP-action proxy                            |
| 6791 | Web dashboard (optional; `docker compose up -d dashboard`) |

## Backend: start / stop

All commands run from `integration/backend/`.

```bash
# start (API + site proxy)
docker compose up -d backend

# also start the dashboard at http://localhost:6791 (optional)
docker compose up -d

# verify it is up
curl http://127.0.0.1:3210/version        # prints a version string

# stop, keeping data in the named volume
docker compose down

# stop and WIPE all data (fresh backend next start)
docker compose down -v
```

## Admin key (local self-hosted)

The self-hosted backend authenticates the CLI with an admin key. Generate one
with the script bundled in the container:

```bash
# from integration/backend/
docker compose exec backend ./generate_admin_key.sh
```

The generated key is stored in `../local.env` as `CONVEX_LOCAL_ADMIN_KEY`.
Regenerate it any time (e.g. after `docker compose down -v`) and update that file.

> The admin key grants full admin access to the LOCAL backend only. It is a
> secret — do not commit `local.env`. (`.gitignore` handling is managed
> separately.)

## Deploy the test project to the LOCAL backend

Run from `convex-test-project/`. The self-hosted env vars point the CLI at the
local Docker backend and take precedence over any cloud config in `.env.local`.

```bash
npm install    # first time only

# bash:
CONVEX_SELF_HOSTED_URL="http://127.0.0.1:3210" \
CONVEX_SELF_HOSTED_ADMIN_KEY="<key from ../local.env>" \
npx convex deploy -y
```

```powershell
# PowerShell:
$env:CONVEX_SELF_HOSTED_URL="http://127.0.0.1:3210"
$env:CONVEX_SELF_HOSTED_ADMIN_KEY="<key from ../local.env>"
npx convex deploy -y
```

Verify (expects `{"status":"success","value":null}`):

```bash
curl -X POST http://127.0.0.1:3210/api/query \
  -H "Content-Type: application/json" \
  -d '{"path":"counters:get","args":[{"name":"smoke"}],"format":"convex_encoded_json"}'
```

## Deploy the test project to the CLOUD dev deployment

Cloud credentials live in `../../../convex.env.local`
(`F:\GitHub-Potionify\potionify-workspace\convex.env.local`): `CONVEX_DEPLOY_KEY`
(a `dev:` deploy key) and `CONVEX_URL`.

Because it is a **dev** deploy key, use `convex dev --once` (a one-shot push to
the dev deployment). `convex deploy` targets production and is not used here.
Make sure the self-hosted env vars from the local flow are **not** set.

```bash
# from convex-test-project/, with CONVEX_DEPLOY_KEY exported from convex.env.local:
export CONVEX_DEPLOY_KEY="<from convex.env.local>"
npx convex dev --once
```

Verify against the cloud URL (expects `{"status":"success","value":null}`):

```bash
curl -X POST https://disciplined-cow-12.convex.cloud/api/query \
  -H "Content-Type: application/json" \
  -H "Authorization: Convex <CONVEX_DEPLOY_KEY from convex.env.local>" \
  -d '{"path":"counters:get","args":[{"name":"smoke"}],"format":"convex_encoded_json"}'
```

> Never paste the deploy key into source or logs — always reference it from the
> env file.

## Env files at a glance

| File                                             | Contents                                             | Secret |
|--------------------------------------------------|------------------------------------------------------|--------|
| `integration/local.env`                          | `CONVEX_LOCAL_URL`, `CONVEX_LOCAL_ADMIN_KEY`         | yes    |
| `integration/convex-test-project/.env.local`     | cloud `CONVEX_DEPLOYMENT` / `CONVEX_URL` (auto-written by `convex dev`) | yes |
| `potionify-workspace/convex.env.local`           | cloud `CONVEX_DEPLOY_KEY`, `CONVEX_URL`              | yes    |

## Functions reference (for the C++ tests)

- `messages:list` (query) — args `{channel}`, returns messages oldest-first.
- `messages:listPaginated` (query) — args `{channel, paginationOpts}`, returns
  a `PaginationResult` page of messages oldest-first (for `paginated_query`).
- `messages:send` (mutation) — args `{channel, author, body}`, returns the new id.
- `messages:clearAll` (mutation) — no args, deletes all, returns count removed.
- `counters:get` (query) — args `{name}`, returns `number | null`.
- `counters:increment` (mutation) — args `{name, by?}`, creates/increments, returns new value.
- `values:echoQuery` / `values:echoMutation` — args `{x}` (`v.any()`), returns `x`.
- `values:kitchenSink` (query) — no args, returns an object with every Convex
  value type: null, booleans, Int64 (`9007199254740993n`, `-9223372036854775808n`),
  floats (incl. `NaN`, `±Infinity`, `-0` in an array), a unicode string, bytes
  (`ArrayBuffer` of `0..7`), a nested array, and a nested object.
- `errors:throwConvexError` (query) — throws `ConvexError({code:"TEST", details:[1,"two"]})`.
- `errors:throwPlainError` (query) — throws `Error("plain failure")`.
- `actions:echoAction` (action) — args `{x}`, returns `x`.
- `actions:now` (action) — no args, returns `Date.now()`.
