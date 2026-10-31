/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Base URL of the TrailCamHub API (e.g. a Lambda function URL). Empty → demo mode. */
  readonly VITE_API_BASE_URL?: string;
  /** Short commit hash shown in the footer. */
  readonly VITE_APP_VERSION?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
