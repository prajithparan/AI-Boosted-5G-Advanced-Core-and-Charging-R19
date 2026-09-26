import { defineConfig } from 'vite';

// No @vitejs/plugin-react: Vite's built-in transform handles TSX (tsconfig "jsx": "react-jsx"),
// which keeps babel/browserslist -- and caniuse-lite (CC-BY-4.0, not OSI) -- out of the tree.
// No dev-server proxy either: the BFF is h2-only + mTLS; `npm run watch` rebuilds dist/ for it.
export default defineConfig({
  build: { outDir: 'dist', emptyOutDir: true, sourcemap: false, modulePreload: { polyfill: false } },
});
