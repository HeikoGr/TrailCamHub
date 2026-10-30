# TrailCamHub backend (AWS SAM)

A small serverless backend for the hub and the Telegram Mini App:

```
Hub (ESP32) ──Bearer <device key>──┐
                                   ├─▶ Lambda (Function URL) ──▶ DynamoDB (cameras, media, commands)
Mini App ──tma <Telegram initData>─┘          │
                                              └─▶ S3 (thumbnails, via presigned URLs)
```

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
| POST   | `/v1/device/checkin`           | `Bearer <camId>.<key>` | Telemetry + media list → uploads + commands  |
| POST   | `/v1/device/ack`               | `Bearer <camId>.<key>` | Confirm uploaded thumbnails and commands     |

### Hub check-in cycle

1. `POST /v1/device/checkin` with telemetry and the camera's media list:

   ```json
   {
     "hubFirmware": "0.4.0",
     "hubBatPercent": 91,
     "camera": { "model": "TC100", "firmware": "2.3.0.3", "batPercent": 78, "sdFreeMB": 28412 },
     "media": [{ "dir": 100, "num": 36, "fileType": 0, "mediaTime": 1766505692 }]
   }
   ```

   The response lists missing thumbnails (newest first, at most 20) with a
   presigned `PUT` URL each, open commands, and the sleep interval:

   ```json
   {
     "serverTime": 1766506000,
     "thumbUploads": [{ "dir": 100, "num": 36, "url": "https://…" }],
     "commands": [{ "id": "…", "type": "requestVideo", "dir": 100, "num": 35 }],
     "config": { "sleepIntervalS": 1800 }
   }
   ```

2. `PUT` each JPEG to its URL (`Content-Type: image/jpeg`).
3. `POST /v1/device/ack` with `{"uploadedThumbs":[{"dir":100,"num":36}], "commands":[{"id":"…","state":"done"}]}`.
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

4. **Register a camera** and link it to your Telegram user id (e.g. from
   [@userinfobot](https://t.me/userinfobot)):

   ```bash
   TABLE=$(aws cloudformation describe-stacks --stack-name trailcamhub \
     --query "Stacks[0].Outputs[?OutputKey=='TableName'].OutputValue" --output text)
   npm run register-camera -- --table "$TABLE" --cam F4C15606ED3B --name Garden --user <id>
   ```

   The printed device key goes onto the hub. Running it again rotates the key.

## Costs

Lambda, DynamoDB (provisioned 5/5) and outbound traffic stay within the AWS
always-free allowances at this scale. S3 storage is billed normally (cents per
month for thumbnails). Set an AWS Budgets alert anyway.

## Not yet included

Telegram bot webhook (notifications, buttons), camera pairing by QR code, video
upload, OTA manifest.
