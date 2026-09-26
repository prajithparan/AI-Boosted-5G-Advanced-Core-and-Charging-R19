// Operator GUI shell (ADR-0420): who am I, then only the screens my permissions allow. Hiding a
// screen is a convenience -- the BFF authorizes and audits every call regardless.
import { useEffect, useState } from 'react';

import { call, type Me } from './api';
import { ApprovalsScreen, CatalogScreen, ConfigScreen, OnboardingScreen } from './screens';

type Tab = 'onboarding' | 'catalog' | 'approvals' | 'config';

export function App() {
  const [me, setMe] = useState<Me | null | 'anonymous'>(null);
  const [tab, setTab] = useState<Tab | null>(null);
  useEffect(() => {
    void call('GET', '/api/me').then((r) => setMe(r.ok ? (r.body as Me) : 'anonymous'));
  }, []);
  if (me === null) return <p>Loading...</p>;
  if (me === 'anonymous') {
    return (
      <main className="login">
        <h1>5gc-r19 operator console</h1>
        <p>Sign in with your operator account (multi-factor authentication is required).</p>
        <a className="button" href="/auth/login">
          Sign in
        </a>
      </main>
    );
  }
  const has = (p: string) => me.permissions.includes(p);
  const tabs: [Tab, string, boolean][] = [
    ['onboarding', 'Onboarding', has('customer_order:create') || has('customer_order:read')],
    ['catalog', 'Catalog', has('product_offering:read')],
    [
      'approvals',
      'Approvals',
      me.permissions.some((p) => p.endsWith('_approval:decide') || p.endsWith(':change') || p === 'product_offering:create'),
    ],
    ['config', 'NF configuration', has('nf_config:read')],
  ];
  const visible = tabs.filter(([, , ok]) => ok);
  const current = tab ?? visible[0]?.[0] ?? null;
  const logout = async () => {
    const r = await call('POST', '/auth/logout', {});
    const url = (r.body as { logoutUrl?: string } | null)?.logoutUrl;
    window.location.href = url || '/';
  };
  return (
    <main>
      <header className="top">
        <h1>5gc-r19 operator console</h1>
        <span>
          {me.displayName} ({me.username}) -- unit {me.orgUnit} -- terminal {me.terminal}
        </span>
        <button onClick={() => void logout()}>Sign out</button>
      </header>
      <nav>
        {visible.map(([id, label]) => (
          <button key={id} className={id === current ? 'active' : ''} onClick={() => setTab(id)}>
            {label}
          </button>
        ))}
      </nav>
      {current === 'onboarding' && <OnboardingScreen />}
      {current === 'catalog' && (
        <CatalogScreen canPropose={has('product_offering:create') || has('product_offering_price:create')} />
      )}
      {current === 'approvals' && <ApprovalsScreen />}
      {current === 'config' && <ConfigScreen canChange={has('nf_config:change')} />}
      {visible.length === 0 && <p>Your account has no GUI permissions. Contact your security administrator.</p>}
    </main>
  );
}
