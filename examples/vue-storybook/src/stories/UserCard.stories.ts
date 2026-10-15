import type { Meta, StoryObj } from '@storybook/vue3';
import UserCard from '../components/UserCard.vue';

const meta = {
  title: 'Components/UserCard',
  component: UserCard,
  tags: ['autodocs'],
} satisfies Meta<typeof UserCard>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: { name: 'Ada Lovelace', role: 'Engineer', online: true },
};

export const Offline: Story = {
  args: { name: 'Grace Hopper', role: 'Admiral' },
};