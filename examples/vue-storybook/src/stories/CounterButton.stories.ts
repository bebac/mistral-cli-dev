import type { Meta, StoryObj } from '@storybook/vue3';
import CounterButton from '../components/CounterButton.vue';

const meta = {
  title: 'Components/CounterButton',
  component: CounterButton,
  tags: ['autodocs'],
  argTypes: {
    step: { control: 'number' },
  },
} satisfies Meta<typeof CounterButton>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const BigSteps: Story = {
  args: { label: 'Score', step: 10 },
};