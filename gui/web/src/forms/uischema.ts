// UI-schema generation from the derived JSON Schemas (ADR-0421). Layout only: every Control's
// scope points at a property that exists in the schema, so the UI can never add a field. We
// generate our own rather than using JSON Forms' default generator because the default one
// (a) emits bare Controls for nested objects, which the vanilla renderer set cannot render, and
// (b) silently drops untyped properties (the DTO's `nlohmann::json` fields), which would make a
// real TMF620 field invisible without saying so.

import type { ControlElement, GroupLayout, JsonSchema7, UISchemaElement, VerticalLayout } from '@jsonforms/core';

export type Schema = JsonSchema7 & { 'x-ui-hidden'?: boolean };

const encode = (k: string) => k.replace(/~/g, '~0').replace(/\//g, '~1');

export const isUntyped = (s: Schema): boolean => s.type === undefined && !s.oneOf && !s.enum;

const humanise = (k: string) =>
  k.replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/^./, (c) => c.toUpperCase());

// Primitives first, then nested objects and arrays, so the common fields sit at the top.
const complex = (s: Schema) => s.type === 'object' || s.type === 'array' || isUntyped(s);

export function controlFor(key: string, s: Schema): ControlElement {
  const c: ControlElement = { type: 'Control', scope: `#/properties/${encode(key)}`, label: humanise(key) };
  if (s.type === 'array' && s.items && (s.items as Schema).type === 'object') {
    // Array items render through this detail layout (ArrayControlRenderer honours options.detail).
    c.options = { detail: layoutFor(s.items as Schema) };
  }
  if (isUntyped(s)) {
    c.options = { ...(c.options ?? {}), json: true };
  }
  return c;
}

export function layoutFor(s: Schema, label?: string): VerticalLayout | GroupLayout {
  const props = Object.entries((s.properties ?? {}) as Record<string, Schema>).filter(
    ([, p]) => !p['x-ui-hidden'],
  );
  const ordered = [...props.filter(([, p]) => !complex(p)), ...props.filter(([, p]) => complex(p))];
  const elements: UISchemaElement[] = ordered.map(([k, p]) => controlFor(k, p));
  return label ? { type: 'Group', label, elements } : { type: 'VerticalLayout', elements };
}
