import type { StorybookConfig } from '@storybook/vue3-vite';

const config: StorybookConfig = {
  "stories": [
    "../src/**/*.stories.@(js|jsx|mjs|ts|tsx)"
  ],
  "addons": [
    "@storybook/addon-mcp",
    "@storybook/addon-vitest",
    "@chromatic-com/storybook"
  ],
  "framework": "@storybook/vue3-vite"
};
export default config;