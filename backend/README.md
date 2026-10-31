# TrailCamHub backend (AWS SAM)

A small serverless backend for the hub and the Telegram Mini App:

```
Hub (ESP32) ──Bearer <hub key>─────┐
                                   ├─▶ Lambda (Function URL) ──▶ DynamoDB (hubs, cameras, media, commands)
Mini App ──tma <Telegram initData>─┘          │
                                              └─▶ S3 (thumbnails, via presigned URLs)
```

## Hubs, cameras and users

- A **hub** is one TrailCamHub module. It has its own key and reports for one or
  more trail cameras.
- A **camera** is created automatically the first time a hub reports it and then
  belongs to that hub. If another hub reports the same camera id, that report is
  refused (`accepted: false`), so a hub cannot take over someone else's camera.
- **Users** (Telegram accounts) are linked to hubs and see every camera of their
  hubs. Several users can share a hub.

- **One Lambda function** (Node.js 22, TypeScript, arm64) behind a **Function URL**
  — no API Gateway needed.
- **DynamoDB** single table with provisioned 5/5 capacity, which stays in the
  always-free tier.
- **S3** bucket for thumbnails. The hub uploads and the app downloads directly via
  short-lived presigned URLs; the bucket stays private.
- The **Telegram bot token** lives in SSM Parameter Store, never in the repository.

Everything is defined in [`template.yaml`](template.yaml) (AWS SAM).

## API v1

| Method | Path                           | Auth                   | Purpose                                      |
| ------ | ------------------------------ | ---------------------- | -------------------------------------------- |
| GET    | `/v1/health`                   | –                      | Liveness check                               |
| GET    | `/v1/cameras`                  | `tma <initData>`       | Cameras linked to the Telegram user          |
| GET    | `/v1/cameras/{camId}/media`    | `tma <initData>`       | Recordings with uploaded thumbnails          |
| POST   | `/v1/cameras/{camId}/commands` | `tma <initData>`       | `{"type":"requestVideo","dir":100,"num":35}` |
| POST   | `/v1/device/checkin`           | `Bearer <hubId>.<key>` | Telemetry + media of all cameras (batch)     |
| POST   | `/v1/device/ack`               | `Bearer <hubId>.<key>` | Confirm uploaded thumbnails and commands     |

### Hub check-in cycle

One check-in per cycle covers all cameras the hub read, so a cycle needs a single
request (and TLS handshake) however many cameras there are — important over LTE.

1. `POST /v1/device/checkin` with the hub's telemetry and one entry per camera:

   ```json
   {
     "hubFirmware": "0.5.0",
     "hubBatPercent": 91,
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "readAt": 1766505900,
         "model": "TC100",
         "firmware": "2.3.0.3",
         "batPercent": 78,
         "sdFreeMB": 28412,
         "media": [{ "dir": 100, "num": 36, "fileType": 0, "mediaTime": 1766505692 }]
       },
       { "camId": "A1B2C3D4E5F6", "batPercent": 40, "media": [] }
     ]
   }
   ```

   - `media` is a **delta**: only recordings the hub has not reported before. The
     backend only adds entries, so repeating one is harmless.
   - `readAt` (hub clock) becomes the camera's "last seen"; implausible values are
     replaced by the server time.
   - Limits per request: 16 cameras, 2000 media entries.

   The response has one entry per reported camera, plus every other camera of the
   hub that has work (missing thumbnails or commands):

   ```json
   {
     "serverTime": 1766506000,
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "accepted": true,
         "resync": false,
         "thumbUploads": [{ "dir": 100, "num": 36, "url": "https://…" }],
         "commands": [{ "id": "…", "type": "requestVideo", "dir": 100, "num": 35 }]
       }
     ],
     "config": { "sleepIntervalS": 1800 }
   }
   ```

   - `thumbUploads`: missing thumbnails, newest first over all cameras, at most 30
     per check-in, each with a presigned `PUT` URL.
   - `resync: true`: the camera is new to the backend; the hub should send its
     full media list at the next check-in.
   - `accepted: false`: the camera belongs to another hub; nothing was stored.

2. `PUT` each JPEG to its URL (`Content-Type: image/jpeg`).
3. `POST /v1/device/ack`, grouped by camera:

   ```json
   {
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "uploadedThumbs": [{ "dir": 100, "num": 36 }],
         "commands": [{ "id": "…", "state": "done" }]
       }
     ]
   }
   ```

   The backend checks S3 before marking a thumbnail as present, and only then does
   it appear in the app. Commands are re-sent at every check-in until acknowledged.

`serverTime` lets the hub set its clock (and the camera's `utcTime`).

## Development

```bash
cd backend
npm install
npm test             # Vitest, uses an in-memory store
npm run lint && npm run typecheck && npm run format:check
sam validate --lint && sam build
```

## One-time AWS setup

Prerequisites: AWS CLI and SAM CLI with admin credentials for your account, region
`eu-central-1`.

1. **Bot token** (from @BotFather) as SecureString:

   ```bash
   aws ssm put-parameter --name /trailcamhub/telegram-bot-token \
     --type SecureString --value '<bot token>'
   ```

2. **GitHub deployment access** (OIDC role, artifact bucket, CloudFormation
   execution role limited to `trailcamhub-*` resources):

   ```bash
   aws cloudformation deploy --stack-name trailcamhub-github-deploy \
     --template-file bootstrap/github-deploy.yaml --capabilities CAPABILITY_IAM
   # add --parameter-overrides CreateOidcProvider=false if the account already
   # has the token.actions.githubusercontent.com provider
   aws cloudformation describe-stacks --stack-name trailcamhub-github-deploy \
     --query 'Stacks[0].Outputs'
   ```

3. Set the outputs as **repository variables** (Settings → Secrets and variables →
   Actions → Variables): `AWS_DEPLOY_ROLE_ARN`, `AWS_CFN_EXECUTION_ROLE_ARN`,
   `AWS_SAM_ARTIFACT_BUCKET`, and optionally `AWS_REGION`.

From then on, every push to `main` that touches `backend/` deploys the stack. The
job summary shows the API URL; set it as `TRAILCAMHUB_API_BASE_URL` so the Mini
App uses the backend instead of demo data.

4. **Register a hub** and link it to your Telegram user id (e.g. from
   [@userinfobot](https://t.me/userinfobot)):

   ```bash
   TABLE=$(aws cloudformation describe-stacks --stack-name trailcamhub \
     --query "Stacks[0].Outputs[?OutputKey=='TableName'].OutputValue" --output text)
   npm run register-hub -- --table "$TABLE" --hub garden-hub --name Garden --user <id>
   ```

   The printed hub key goes onto the hub (web installer → Device setup → Device
   key). Cameras need no registration: they appear when the hub first reports
   them. Add family members with `--user <id>` (repeatable); `--rotate-key`
   issues a new key.

## Costs

Lambda, DynamoDB (provisioned 5/5) and outbound traffic stay within the AWS
always-free allowances at this scale. S3 storage is billed normally (cents per
month for thumbnails). Set an AWS Budgets alert anyway.

## Not yet included

Telegram bot webhook (notifications, buttons), camera pairing by QR code, video
upload, OTA manifest.
