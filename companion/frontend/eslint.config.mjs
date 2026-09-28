// ESLint for the companion's frontend - mainly to catch references to
// names that no longer exist (no-undef): a leftover `initialPage` from
// v0.4.2 threw at startup and left the Map page black on Windows until
// v0.4.5, and nothing flagged it.
import js from '@eslint/js';
import globals from 'globals';

export default [
  { ignores: ['dist/', 'wailsjs/'] },
  js.configs.recommended,
  {
    files: ['**/*.js'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: globals.browser,
    },
    rules: {
      // `catch (e) { /* fall back */ }` is used on purpose throughout.
      'no-unused-vars': ['error', { caughtErrors: 'none' }],
    },
  },
];
