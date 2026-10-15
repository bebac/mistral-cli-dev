import type { Meta, StoryObj } from '@storybook/vue3';
import TodoList from '../components/TodoList.vue';

const meta = {
  title: 'Components/TodoList',
  component: TodoList,
  tags: ['autodocs'],
} satisfies Meta<typeof TodoList>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: {
    title: 'Today',
    initialTodos: [
      { text: 'Scaffold test project', done: true },
      { text: 'Explore Storybook MCP', done: false },
    ],
  },
};

export const Empty: Story = {
  args: { title: 'Backlog' },
};