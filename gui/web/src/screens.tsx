// The first-increment screens (ADR-0420..0425): onboarding (shop agents), catalog proposals,
// approvals (checkers), NF configuration. Each is shown only if /api/me lists a permission it
// needs -- a convenience; the BFF enforces and audits every call regardless.

import { useCallback, useEffect, useState } from 'react';

import { call, type ApiResult } from './api';
import { prune, SchemaForm } from './forms/SchemaForm';
import type { Schema } from './forms/uischema';
import { validate } from './forms/validate';
import orderSchemaJson from './schemas/provisioning-customer-order.schema.json';
import tmf620 from './schemas/tmf620.derived.json';

const orderSchema = orderSchemaJson as Schema;
const nfSchemas = import.meta.glob('./schemas/nf-config/*.schema.json', { eager: true, import: 'default' }) as Record<
  string,
  Schema
>;

const valid = (schema: Schema, data: unknown): string[] => validate(schema, data);

function Result({ r }: { r: ApiResult | null }) {
  if (!r) return null;
  return (
    <div className={r.ok ? 'result ok' : 'result err'}>
      <strong>HTTP {r.status}</strong>
      <pre>{JSON.stringify(r.body, null, 2)}</pre>
    </div>
  );
}

// ---- onboarding -------------------------------------------------------------------------------

export function OnboardingScreen() {
  const [data, setData] = useState<unknown>({});
  const [result, setResult] = useState<ApiResult | null>(null);
  const [errors, setErrors] = useState<string[]>([]);
  const [lookup, setLookup] = useState('');
  const [orders, setOrders] = useState<string[]>([]); // this browser tab only, never persisted
  const [schema, setSchema] = useState<Schema>(orderSchema);

  // Runtime augmentation, labelled as such: offer the catalog's existing offerings as choices
  // (the service's foreign key accepts nothing else). Falls back to free text if the list fails.
  useEffect(() => {
    void call('GET', '/api/tmf620/productOffering').then((r) => {
      if (!r.ok || !Array.isArray(r.body) || r.body.length === 0) return;
      const oneOf = (r.body as { id: string; name?: string }[]).map((o) => ({ const: o.id, title: `${o.name ?? o.id} (${o.id})` }));
      const s = structuredClone(orderSchema) as Schema & { properties: Record<string, Schema> };
      s.properties.productOfferingId = { ...s.properties.productOfferingId, oneOf };
      setSchema(s);
    });
  }, []);

  const submit = useCallback(async () => {
    const body = prune(data) ?? {};
    const errs = valid(schema, body);
    setErrors(errs);
    if (errs.length) return;
    const r = await call('POST', '/api/provisioning/customerOrder', body);
    // SIM keys leave the form state the moment the order is sent, whatever the outcome.
    setData((d: unknown) => {
      const copy = structuredClone(d ?? {}) as { sim?: Record<string, unknown> };
      if (copy.sim) {
        delete copy.sim.k;
        delete copy.sim.opc;
      }
      return copy;
    });
    setResult(r);
    const id = (r.body as { orderId?: string } | null)?.orderId;
    if (id) setOrders((o) => [id, ...o]);
  }, [data, schema]);

  return (
    <section>
      <h2>Customer onboarding</h2>
      <p className="note">
        Orders are created for your shop only. SIM K and OPc are sent once to the provisioning service and are never shown,
        stored or logged by this GUI. Customer identifiers in responses are masked.
      </p>
      <SchemaForm schema={schema} data={data} onChange={(d) => setData(d)} />
      {errors.length > 0 && (
        <ul className="errors">
          {errors.map((e) => (
            <li key={e}>{e}</li>
          ))}
        </ul>
      )}
      <button onClick={() => void submit()}>Submit order</button>
      <Result r={result} />
      <h3>Look up an order</h3>
      <input value={lookup} onChange={(e) => setLookup(e.target.value)} placeholder="order id, e.g. ord-shop-a.123" />
      <button onClick={() => void call('GET', `/api/provisioning/customerOrder/${encodeURIComponent(lookup)}`).then(setResult)}>
        Get
      </button>
      {orders.length > 0 && <p>Created in this session: {orders.join(', ')}</p>}
    </section>
  );
}

// ---- catalog ----------------------------------------------------------------------------------

const catalogResources = {
  productOffering: tmf620.definitions.ProductOffering as Schema,
  productOfferingPrice: tmf620.definitions.ProductOfferingPrice as Schema,
};

export function CatalogScreen({ canPropose }: { canPropose: boolean }) {
  const [res, setRes] = useState<keyof typeof catalogResources>('productOffering');
  const [list, setList] = useState<ApiResult | null>(null);
  const [data, setData] = useState<unknown>({});
  const [reason, setReason] = useState('');
  const [result, setResult] = useState<ApiResult | null>(null);
  const [errors, setErrors] = useState<string[]>([]);
  const refresh = useCallback(() => void call('GET', `/api/tmf620/${res}`).then(setList), [res]);
  useEffect(refresh, [refresh]);

  const propose = async () => {
    const body = prune(data) ?? {};
    const errs = valid(catalogResources[res], body);
    if (!reason.trim()) errs.push('a change reason is required (four-eyes review)');
    setErrors(errs);
    if (errs.length) return;
    setResult(await call('POST', `/api/tmf620/${res}`, body, { 'x-oam-reason': encodeURIComponent(reason) }));
  };
  const rows = Array.isArray(list?.body) ? (list?.body as Record<string, unknown>[]) : [];
  return (
    <section>
      <h2>Product catalog (TMF620)</h2>
      <select value={res} onChange={(e) => setRes(e.target.value as keyof typeof catalogResources)}>
        <option value="productOffering">ProductOffering</option>
        <option value="productOfferingPrice">ProductOfferingPrice</option>
      </select>
      <button onClick={refresh}>Refresh</button>
      <table>
        <thead>
          <tr>
            <th>id</th>
            <th>name</th>
            <th>lifecycleStatus</th>
            <th>version</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <tr key={String(r.id)}>
              <td>{String(r.id ?? '')}</td>
              <td>{String(r.name ?? '')}</td>
              <td>{String(r.lifecycleStatus ?? '')}</td>
              <td>{String(r.version ?? '')}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {canPropose && (
        <>
          <h3>Propose a new {res} (needs a second person's approval)</h3>
          <SchemaForm key={res} schema={catalogResources[res]} data={data} onChange={(d) => setData(d)} />
          <label>
            Change reason * <input value={reason} onChange={(e) => setReason(e.target.value)} />
          </label>
          {errors.length > 0 && (
            <ul className="errors">
              {errors.map((e) => (
                <li key={e}>{e}</li>
              ))}
            </ul>
          )}
          <button onClick={() => void propose()}>Submit for approval</button>
          <Result r={result} />
        </>
      )}
    </section>
  );
}

// ---- approvals --------------------------------------------------------------------------------

interface Approval {
  id: string;
  permission: string;
  target: string;
  requestedBy: string;
  requestedAt: string;
  reason: string;
  status: string;
  canDecide: boolean;
  payload: unknown;
}

export function ApprovalsScreen() {
  const [items, setItems] = useState<Approval[]>([]);
  const [result, setResult] = useState<ApiResult | null>(null);
  const [why, setWhy] = useState<Record<string, string>>({});
  const refresh = useCallback(
    () => void call('GET', '/api/approvals').then((r) => setItems(Array.isArray(r.body) ? (r.body as Approval[]) : [])),
    [],
  );
  useEffect(refresh, [refresh]);
  const decide = async (id: string, decision: 'approve' | 'reject') => {
    setResult(await call('POST', `/api/approvals/${id}/decision`, { decision, reason: why[id] ?? '' }));
    refresh();
  };
  return (
    <section>
      <h2>Approvals (four-eyes)</h2>
      <p className="note">You never see a Decide button on your own requests; the backend and the database both refuse it.</p>
      <button onClick={refresh}>Refresh</button>
      {items.map((a) => (
        <article key={a.id} className="approval">
          <header>
            <strong>{a.permission}</strong> {a.target} -- {a.status} -- by {a.requestedBy} at {a.requestedAt}
          </header>
          <p>Reason: {a.reason}</p>
          <details>
            <summary>Proposed content</summary>
            <pre>{JSON.stringify(a.payload, null, 2)}</pre>
          </details>
          {a.canDecide && (
            <div>
              <input placeholder="decision reason *" value={why[a.id] ?? ''} onChange={(e) => setWhy({ ...why, [a.id]: e.target.value })} />
              <button onClick={() => void decide(a.id, 'approve')}>Approve</button>
              <button onClick={() => void decide(a.id, 'reject')}>Reject</button>
            </div>
          )}
        </article>
      ))}
      <Result r={result} />
    </section>
  );
}

// ---- NF configuration -------------------------------------------------------------------------

interface ConfigView {
  nf: string;
  editable: boolean;
  apply: string;
  content: unknown;
  versions: { version: number; createdBy: string; createdAt: string; appliedAt: string; comment: string }[];
}

export function ConfigScreen({ canChange }: { canChange: boolean }) {
  const all = Object.keys(nfSchemas)
    .map((p) => p.replace(/^.*\/(.*)\.schema\.json$/, '$1'))
    .sort();
  const [nf, setNf] = useState('product-catalog');
  const [view, setView] = useState<ConfigView | null>(null);
  const [data, setData] = useState<unknown>(null);
  const [reason, setReason] = useState('');
  const [result, setResult] = useState<ApiResult | null>(null);
  const schema = nfSchemas[`./schemas/nf-config/${nf}.schema.json`];
  const load = useCallback(
    () =>
      void call('GET', `/api/config/${nf}`).then((r) => {
        setResult(r.ok ? null : r);
        const v = r.ok ? (r.body as ConfigView) : null;
        setView(v);
        setData(v?.content ?? null);
      }),
    [nf],
  );
  useEffect(load, [load]);
  const review = (schema as { 'x-review'?: string[] })['x-review'] ?? [];
  return (
    <section>
      <h2>Network function configuration</h2>
      <p className="note">
        Schemas are derived from each component's config file and source. Every component reads its configuration at
        start-up only: an applied change is written to the file and takes effect on the next restart.
      </p>
      <select value={nf} onChange={(e) => setNf(e.target.value)}>
        {all.map((n) => (
          <option key={n}>{n}</option>
        ))}
      </select>
      {review.length > 0 && (
        <details>
          <summary>{review.length} open review item(s) in this schema</summary>
          <ul>
            {review.map((r) => (
              <li key={r}>{r}</li>
            ))}
          </ul>
        </details>
      )}
      {view && schema && (
        <>
          <p>
            {view.editable ? 'Editable (four-eyes)' : 'Read-only in this increment'} -- applies on: {view.apply}
          </p>
          <SchemaForm schema={schema} data={data} readonly={!view.editable || !canChange} onChange={(d) => setData(d)} />
          {view.editable && canChange && (
            <>
              <label>
                Change reason * <input value={reason} onChange={(e) => setReason(e.target.value)} />
              </label>
              <button onClick={() => void call('POST', `/api/config/${nf}`, { content: data, reason }).then(setResult)}>
                Submit change for approval
              </button>
            </>
          )}
          <h3>Versions</h3>
          <table>
            <thead>
              <tr>
                <th>v</th>
                <th>by</th>
                <th>created</th>
                <th>applied</th>
                <th>comment</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {view.versions.map((v) => (
                <tr key={v.version}>
                  <td>{v.version}</td>
                  <td>{v.createdBy}</td>
                  <td>{v.createdAt}</td>
                  <td>{v.appliedAt}</td>
                  <td>{v.comment}</td>
                  <td>
                    {view.editable && canChange && (
                      <button
                        onClick={() =>
                          void call('POST', `/api/config/${nf}/rollback`, {
                            version: v.version,
                            reason: reason || `rollback to v${v.version}`,
                          }).then(setResult)
                        }
                      >
                        Propose rollback
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
      <Result r={result} />
    </section>
  );
}
