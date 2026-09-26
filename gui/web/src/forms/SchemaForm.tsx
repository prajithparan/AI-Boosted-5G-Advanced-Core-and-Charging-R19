// One JSON-Schema-driven form (ADR-0421): the schema is the derived one, the layout is generated
// from it. Required fields carry "*". Validation is ./validate.ts (see there for why not AJV).

import { JsonForms } from '@jsonforms/react';
import { useMemo } from 'react';

import { cells, renderers } from './renderers';
import { layoutFor, type Schema } from './uischema';

export interface SchemaFormProps {
  schema: Schema;
  data: unknown;
  onChange: (data: unknown) => void;
  readonly?: boolean;
}

export function SchemaForm({ schema, data, onChange, readonly }: SchemaFormProps) {
  const uischema = useMemo(() => layoutFor(schema), [schema]);
  return (
    <div className="schema-form">
      <p className="legend">Fields marked * are required by the service.</p>
      <JsonForms
        schema={schema}
        uischema={uischema}
        data={data}
        renderers={renderers}
        cells={cells}
        readonly={readonly}
        // AJV needs eval, which the console's CSP forbids: validation is ./validate.ts on submit.
        validationMode="NoValidation"
        onChange={({ data: d }) => onChange(d)}
      />
    </div>
  );
}

/** Drops empty objects/arrays/strings a form leaves behind, so an untouched optional group
 *  is not sent as `{}` (which would then fail its own nested `required`). */
export function prune(v: unknown): unknown {
  if (Array.isArray(v)) {
    const a = v.map(prune).filter((x) => x !== undefined);
    return a.length ? a : undefined;
  }
  if (v && typeof v === 'object') {
    const o: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v)) {
      const p = prune(x);
      if (p !== undefined) o[k] = p;
    }
    return Object.keys(o).length ? o : undefined;
  }
  if (v === '' || v === null) return undefined;
  return v;
}
