// Browser -> oam-gui-bff calls (ADR-0422). Same origin only; the operator's client certificate
// authenticates the TLS connection, and the custom header is the BFF's CSRF gate.

export interface ApiResult {
  status: number;
  ok: boolean;
  body: unknown; // parsed JSON, or the raw text if the body is not JSON
}

async function call(method: 'GET' | 'POST', path: string, body?: unknown): Promise<ApiResult> {
  const headers: Record<string, string> = { accept: 'application/json' };
  let payload: string | undefined;
  if (body !== undefined) {
    headers['content-type'] = 'application/json';
    headers['x-requested-by'] = 'oam-gui';
    payload = JSON.stringify(body);
  }
  let resp: Response;
  try {
    resp = await fetch(path, {
      method,
      headers,
      body: payload,
      credentials: 'same-origin',
      cache: 'no-store',
    });
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

export const api = {
  list: (collection: string) => call('GET', `/api/${collection}`),
  get: (collection: string, id: string) => call('GET', `/api/${collection}/${encodeURIComponent(id)}`),
  create: (collection: string, body: unknown) => call('POST', `/api/${collection}`, body),
};

export const TMF620_OFFERING = 'tmf620/productOffering';
export const TMF620_PRICE = 'tmf620/productOfferingPrice';
export const CUSTOMER_ORDER = 'provisioning/customerOrder';
