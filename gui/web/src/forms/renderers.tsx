// The three renderers the vanilla JSON Forms set lacks for these schemas (ADR-0421):
//   * ObjectControl  -- a nested object (validFor, productSpecification, sim, ...) as a fieldset;
//   * SecretCell     -- a `writeOnly` string (SIM K / OPc): masked, never autofilled or saved;
//   * JsonCell       -- a property the DTO keeps untyped (nlohmann::json), edited as raw JSON.
// Plus a re-rank so every array of objects uses the nesting-capable ArrayControlRenderer.

import {
  and,
  isControl,
  isObjectArrayControl,
  isObjectControl,
  rankWith,
  schemaMatches,
  type CellProps,
  type ControlProps,
  type JsonFormsCellRendererRegistryEntry,
  type JsonFormsRendererRegistryEntry,
  type StatePropsOfControlWithDetail,
} from '@jsonforms/core';
import {
  JsonFormsDispatch,
  withJsonFormsCellProps,
  withJsonFormsDetailProps,
} from '@jsonforms/react';
import { ArrayControl, vanillaCells, vanillaRenderers } from '@jsonforms/vanilla-renderers';
import { useState } from 'react';

import { isUntyped, layoutFor, type Schema } from './uischema';

// ---- nested object ---------------------------------------------------------------------------

function ObjectControl(props: StatePropsOfControlWithDetail) {
  const { schema, path, visible, renderers, cells, enabled, label, required } = props;
  if (!visible) return null;
  return (
    <fieldset className="object-control">
      <legend>
        {label}
        {required ? ' *' : ''}
      </legend>
      <JsonFormsDispatch
        schema={schema}
        uischema={layoutFor(schema as Schema)}
        path={path}
        renderers={renderers}
        cells={cells}
        enabled={enabled}
      />
    </fieldset>
  );
}

// ---- SIM keys --------------------------------------------------------------------------------

// A text input with CSS masking rather than type=password: browsers offer to save (and later
// autofill) anything typed into a password field, which would persist the SIM key in the
// operator's password store -- exactly what ADR-0422 forbids. autoComplete=off + no spellcheck
// keep it out of form history and remote spell services too.
function SecretCell(props: CellProps) {
  const { data, id, enabled, path, handleChange } = props;
  return (
    <input
      type="text"
      className="secret"
      id={id}
      value={(data as string | undefined) ?? ''}
      disabled={!enabled}
      autoComplete="off"
      spellCheck={false}
      autoCorrect="off"
      autoCapitalize="off"
      data-lpignore="true"
      data-1p-ignore="true"
      onChange={(ev) => handleChange(path, ev.target.value === '' ? undefined : ev.target.value)}
    />
  );
}

const isWriteOnlyString = schemaMatches(
  (s) => (s as Schema).type === 'string' && (s as Schema).writeOnly === true,
);

// ---- untyped JSON ----------------------------------------------------------------------------

function JsonCell(props: CellProps) {
  const { data, id, enabled, path, handleChange } = props;
  const [text, setText] = useState(data === undefined ? '' : JSON.stringify(data));
  const [bad, setBad] = useState(false);
  return (
    <>
      <textarea
        id={id}
        className={bad ? 'json invalid' : 'json'}
        value={text}
        disabled={!enabled}
        placeholder="any JSON value, e.g. 5 or &quot;gold&quot; or {&quot;sst&quot;:1}"
        onChange={(ev) => {
          setText(ev.target.value);
          if (ev.target.value.trim() === '') {
            setBad(false);
            handleChange(path, undefined);
            return;
          }
          try {
            handleChange(path, JSON.parse(ev.target.value));
            setBad(false);
          } catch {
            setBad(true);
          }
        }}
      />
      {bad ? <div className="validation">not valid JSON (the last valid value is kept)</div> : null}
    </>
  );
}

const isUntypedControl = and(isControl, schemaMatches((s) => isUntyped(s as Schema)));

// ---- registries ------------------------------------------------------------------------------

export const renderers: JsonFormsRendererRegistryEntry[] = [
  ...vanillaRenderers,
  { tester: rankWith(5, isObjectControl), renderer: withJsonFormsDetailProps(ObjectControl) },
  { tester: rankWith(5, isObjectArrayControl), renderer: ArrayControl },
];

export const cells: JsonFormsCellRendererRegistryEntry[] = [
  ...vanillaCells,
  { tester: rankWith(10, and(isControl, isWriteOnlyString)), cell: withJsonFormsCellProps(SecretCell) },
  { tester: rankWith(10, isUntypedControl), cell: withJsonFormsCellProps(JsonCell) },
];

export type { ControlProps };
