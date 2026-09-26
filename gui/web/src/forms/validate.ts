// Client-side validation against the derived schemas (ADR-0421), WITHOUT AJV.
//
// Why not AJV (JSON Forms' default): AJV compiles every schema to a function with `new Function`,
// which the BFF's Content-Security-Policy (`script-src 'self'`, no 'unsafe-eval') forbids -- and
// weakening the CSP of an operator console to make a form validate is the wrong trade. The derived
// schemas use a small, closed keyword set, which this interpreter covers exactly: type, properties,
// required, additionalProperties:false, items, enum, oneOf-of-const, pattern, minLength,
// format:date-time. An unknown keyword is ignored here, and the SERVICE remains the authority: the
// BFF and the NF validate again. Error texts carry paths and rules, never values.

import type { Schema } from './uischema';

const RFC3339 = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})$/;

function typeOk(t: string, v: unknown): boolean {
  switch (t) {
    case 'string':
      return typeof v === 'string';
    case 'integer':
      return typeof v === 'number' && Number.isInteger(v);
    case 'number':
      return typeof v === 'number' && Number.isFinite(v);
    case 'boolean':
      return typeof v === 'boolean';
    case 'object':
      return typeof v === 'object' && v !== null && !Array.isArray(v);
    case 'array':
      return Array.isArray(v);
    default:
      return true;
  }
}

export function validate(schema: Schema, value: unknown, path = ''): string[] {
  const at = path || '(root)';
  const errs: string[] = [];
  if (typeof schema.type === 'string' && !typeOk(schema.type, value)) return [`${at}: must be ${schema.type}`];
  const allowed = schema.enum ?? (schema.oneOf?.every((o) => 'const' in (o as object)) ? schema.oneOf.map((o) => (o as { const: unknown }).const) : undefined);
  if (allowed && !allowed.includes(value as never)) errs.push(`${at}: must be one of the listed values`);
  if (typeof value === 'string') {
    if (schema.pattern && !new RegExp(schema.pattern).test(value)) errs.push(`${at}: does not match ${schema.pattern}`);
    if (schema.minLength !== undefined && value.length < schema.minLength) errs.push(`${at}: must not be empty`);
    if (schema.format === 'date-time' && !RFC3339.test(value)) errs.push(`${at}: must be an RFC 3339 date-time`);
  }
  if (typeOk('object', value) && schema.properties) {
    const obj = value as Record<string, unknown>;
    for (const r of (schema.required as string[] | undefined) ?? []) {
      if (obj[r] === undefined) errs.push(`${path ? path + '.' : ''}${r}: required`);
    }
    for (const [k, v] of Object.entries(obj)) {
      const sub = (schema.properties as Record<string, Schema>)[k];
      const p = path ? `${path}.${k}` : k;
      if (!sub) {
        if (schema.additionalProperties === false) errs.push(`${p}: not a field of this resource`);
        continue;
      }
      errs.push(...validate(sub, v, p));
    }
  }
  if (Array.isArray(value) && schema.items && !Array.isArray(schema.items)) {
    value.forEach((v, i) => errs.push(...validate(schema.items as Schema, v, `${path}[${i}]`)));
  }
  return errs;
}
