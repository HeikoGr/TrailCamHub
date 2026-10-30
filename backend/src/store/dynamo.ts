import { ConditionalCheckFailedException, DynamoDBClient } from '@aws-sdk/client-dynamodb';
import {
  DynamoDBDocumentClient,
  GetCommand,
  PutCommand,
  QueryCommand,
  UpdateCommand,
  type QueryCommandInput,
} from '@aws-sdk/lib-dynamodb';
import type { CameraRecord, CommandRecord, MediaRecord, VideoState } from '../model';
import type { Store, Telemetry } from './types';

/*
 * Single-table layout (PK / SK):
 *   CAM#<camId>  / META                   camera record
 *   USER#<id>    / CAM#<camId>            user ↔ camera link
 *   CAM#<camId>  / MEDIA#<dir>#<num>      media (zero-padded, sorts by file)
 *   CAM#<camId>  / CMD#<id>               command
 */
const camPk = (camId: string) => `CAM#${camId}`;
const userPk = (userId: number) => `USER#${userId}`;
const mediaSk = (dir: number, num: number) =>
  `MEDIA#${String(dir).padStart(5, '0')}#${String(num).padStart(6, '0')}`;
const cmdSk = (id: string) => `CMD#${id}`;

function strip<T>(item: Record<string, unknown>): T {
  const { PK: _pk, SK: _sk, ...rest } = item;
  void _pk;
  void _sk;
  return rest as T;
}

export class DynamoStore implements Store {
  private readonly db: DynamoDBDocumentClient;

  constructor(
    private readonly table: string,
    client: DynamoDBClient = new DynamoDBClient({}),
  ) {
    this.db = DynamoDBDocumentClient.from(client, {
      marshallOptions: { removeUndefinedValues: true },
    });
  }

  async putCamera(camera: CameraRecord): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: camPk(camera.camId), SK: 'META', ...camera },
      }),
    );
  }

  async getCamera(camId: string): Promise<CameraRecord | undefined> {
    const { Item } = await this.db.send(
      new GetCommand({ TableName: this.table, Key: { PK: camPk(camId), SK: 'META' } }),
    );
    return Item ? strip<CameraRecord>(Item) : undefined;
  }

  async updateTelemetry(camId: string, telemetry: Telemetry, lastSeen: number): Promise<void> {
    const fields: Record<string, unknown> = { ...telemetry, lastSeen };
    const entries = Object.entries(fields).filter(([, value]) => value !== undefined);
    await this.db.send(
      new UpdateCommand({
        TableName: this.table,
        Key: { PK: camPk(camId), SK: 'META' },
        ConditionExpression: 'attribute_exists(PK)',
        UpdateExpression: 'SET ' + entries.map((_, i) => `#f${i} = :v${i}`).join(', '),
        ExpressionAttributeNames: Object.fromEntries(entries.map(([key], i) => [`#f${i}`, key])),
        ExpressionAttributeValues: Object.fromEntries(
          entries.map(([, value], i) => [`:v${i}`, value]),
        ),
      }),
    );
  }

  async linkUser(userId: number, camId: string): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: userPk(userId), SK: `CAM#${camId}`, camId },
      }),
    );
  }

  async listUserCameraIds(userId: number): Promise<string[]> {
    const items = await this.queryAll({
      TableName: this.table,
      KeyConditionExpression: 'PK = :pk AND begins_with(SK, :sk)',
      ExpressionAttributeValues: { ':pk': userPk(userId), ':sk': 'CAM#' },
    });
    return items.map((item) => String(item.camId));
  }

  async isLinked(userId: number, camId: string): Promise<boolean> {
    const { Item } = await this.db.send(
      new GetCommand({ TableName: this.table, Key: { PK: userPk(userId), SK: `CAM#${camId}` } }),
    );
    return Item !== undefined;
  }

  async listMedia(camId: string): Promise<MediaRecord[]> {
    const items = await this.queryAll({
      TableName: this.table,
      KeyConditionExpression: 'PK = :pk AND begins_with(SK, :sk)',
      ExpressionAttributeValues: { ':pk': camPk(camId), ':sk': 'MEDIA#' },
    });
    return items.map((item) => strip<MediaRecord>(item));
  }

  async getMedia(camId: string, dir: number, num: number): Promise<MediaRecord | undefined> {
    const { Item } = await this.db.send(
      new GetCommand({ TableName: this.table, Key: { PK: camPk(camId), SK: mediaSk(dir, num) } }),
    );
    return Item ? strip<MediaRecord>(Item) : undefined;
  }

  async addMedia(camId: string, items: MediaRecord[]): Promise<void> {
    for (const item of items) {
      try {
        await this.db.send(
          new PutCommand({
            TableName: this.table,
            Item: { PK: camPk(camId), SK: mediaSk(item.dir, item.num), ...item },
            ConditionExpression: 'attribute_not_exists(PK)',
          }),
        );
      } catch (error) {
        if (!(error instanceof ConditionalCheckFailedException)) throw error;
      }
    }
  }

  async markThumbPresent(camId: string, dir: number, num: number): Promise<void> {
    await this.updateIfExists(camId, mediaSk(dir, num), 'thumbPresent', true);
  }

  async setVideoState(camId: string, dir: number, num: number, state: VideoState): Promise<void> {
    await this.updateIfExists(camId, mediaSk(dir, num), 'videoState', state);
  }

  async putCommand(camId: string, command: CommandRecord): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: camPk(camId), SK: cmdSk(command.id), ...command },
      }),
    );
  }

  async getCommand(camId: string, id: string): Promise<CommandRecord | undefined> {
    const { Item } = await this.db.send(
      new GetCommand({ TableName: this.table, Key: { PK: camPk(camId), SK: cmdSk(id) } }),
    );
    return Item ? strip<CommandRecord>(Item) : undefined;
  }

  async listCommands(camId: string): Promise<CommandRecord[]> {
    const items = await this.queryAll({
      TableName: this.table,
      KeyConditionExpression: 'PK = :pk AND begins_with(SK, :sk)',
      ExpressionAttributeValues: { ':pk': camPk(camId), ':sk': 'CMD#' },
    });
    return items.map((item) => strip<CommandRecord>(item)).sort((a, b) => a.created - b.created);
  }

  private async updateIfExists(camId: string, sk: string, field: string, value: unknown) {
    try {
      await this.db.send(
        new UpdateCommand({
          TableName: this.table,
          Key: { PK: camPk(camId), SK: sk },
          ConditionExpression: 'attribute_exists(PK)',
          UpdateExpression: 'SET #f = :v',
          ExpressionAttributeNames: { '#f': field },
          ExpressionAttributeValues: { ':v': value },
        }),
      );
    } catch (error) {
      if (!(error instanceof ConditionalCheckFailedException)) throw error;
    }
  }

  private async queryAll(input: QueryCommandInput): Promise<Record<string, unknown>[]> {
    const items: Record<string, unknown>[] = [];
    let start: QueryCommandInput['ExclusiveStartKey'];
    do {
      const page = await this.db.send(new QueryCommand({ ...input, ExclusiveStartKey: start }));
      items.push(...(page.Items ?? []));
      start = page.LastEvaluatedKey;
    } while (start);
    return items;
  }
}
