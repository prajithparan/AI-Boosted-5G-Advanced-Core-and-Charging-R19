// Browser -> oam-gui-bff calls (ADR-0422..0425). Same origin only. The terminal's client
// certificate authenticates the TLS connection, the HttpOnly session cookie identifies the
// operator, and the custom header is the BFF's CSRF gate. The browser never decides access: every
// call is authorized (and audited) by the BFF.

export interface ApiResult {
  status: number;
  ok: boolean;
  body: unknown; // parsed JSON, or the raw text if the body is not JSON
}

export async function call(
  method: 'GET' | 'POST',
  path: string,
  body?: unknown,
  extraHeaders: Record<string, string> = {},
): Promise<ApiResult> {
  const headers: Record<string, string> = { accept: 'application/json', ...extraHeaders };
  let payload: string | undefined;
  if (method === 'POST') {
    headers['content-type'] = 'application/json';
    headers['x-requested-by'] = 'oam-gui';
    payload = JSON.stringify(body ?? {});
  }
  let resp: Response;
  try {
    resp = await fetch(path, { method, headers, body: payload, credentials: 'same-origin', cache: 'no-store' });
  } catch {
    return { status: 0, ok: false, body: { title: 'Network error', detail: 'the GUI backend is unreachable' } };
  } finally {
    payload = undefined; // drop our reference to a body that may carry SIM keys
  }
  const text = await resp.text();
  let parsed: unknown = text;
  try {
    parsed = text === '' ? null : JSON.parse(text);
  } catch {
    /* keep the text */
  }
  return { status: resp.status, ok: resp.ok, body: parsed };
}

export interface Me {
  userId: string;
  username: string;
  displayName: string;
  orgUnit: string;
  terminal: string;
  permissions: string[];
}
