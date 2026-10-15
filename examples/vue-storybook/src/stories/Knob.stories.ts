import type { Meta, StoryObj } from '@storybook/vue3-vite';
import { expect, fn, userEvent } from 'storybook/test';
import { ref } from 'vue';
import Knob from '../components/Knob.vue';

/**
 * A rotary knob that is adjusted by dragging it vertically with the mouse
 * (or a finger): upwards increases the value, downwards decreases it.
 * A click without dragging does not change the value.
 */
const meta = {
  title: 'Components/Knob',
  component: Knob,
  tags: ['autodocs'],
  args: {
    modelValue: 50,
    label: 'Volume',
    min: 0,
    max: 100,
    step: 1,
    size: 64,
    disabled: false,
  },
  argTypes: {
    modelValue: { control: 'number' },
    min: { control: 'number' },
    max: { control: 'number' },
    step: { control: 'number' },
    size: { control: 'number' },
    disabled: { control: 'boolean' },
  },
} satisfies Meta<typeof Knob>;

export default meta;
type Story = StoryObj<typeof meta>;

/**
 * Wraps the knob with a local v-model so that drags and key presses
 * are visible in the rendered story.
 */
function renderInteractive(args: Record<string, any>) {
  return {
    components: { Knob },
    setup() {
      const value = ref(args.modelValue);
      return { args, value };
    },
    template: `
      <Knob
        v-model="value"
        :label="args.label"
        :min="args.min"
        :max="args.max"
        :step="args.step"
        :size="args.size"
        :disabled="args.disabled"
        @change="args.onChange"
      />
    `,
  };
}

export const Default: Story = {
  render: (args) => renderInteractive(args),
};

export const GainControl: Story = {
  args: {
    modelValue: 0,
    label: 'Gain',
    min: -24,
    max: 24,
    step: 2,
    size: 96,
  },
  render: (args) => renderInteractive(args),
};

export const MouseDragAdjust: Story = {
  args: { onChange: fn() },
  render: (args) => renderInteractive(args),
  play: async ({ args, canvas }) => {
    const dial = canvas.getByRole('slider');
    await expect(canvas.getByText('Volume: 50')).toBeInTheDocument();

    const rect = dial.getBoundingClientRect();
    const centerX = rect.left + rect.width / 2;
    const centerY = rect.top + rect.height / 2;

    // A click without dragging must not change the value.
    await userEvent.pointer([
      {
        keys: '[MouseLeft>]',
        target: dial,
        coords: { clientX: centerX, clientY: rect.bottom },
      },
      { keys: '[/MouseLeft]' },
    ]);
    await expect(canvas.getByText('Volume: 50')).toBeInTheDocument();
    await expect(args.onChange).not.toHaveBeenCalled();

    // Dragging upwards from the centre increases the value.
    await userEvent.pointer([
      {
        keys: '[MouseLeft>]',
        target: dial,
        coords: { clientX: centerX, clientY: centerY },
      },
      {
        target: dial,
        coords: { clientX: centerX, clientY: centerY - 75 },
      },
      { keys: '[/MouseLeft]' },
    ]);
    await expect(canvas.getByText('Volume: 100')).toBeInTheDocument();
    await expect(args.onChange).toHaveBeenCalledWith(100);

    // The dial must not shift sideways while the readout text changes width.
    const rectAfter = dial.getBoundingClientRect();
    await expect(rectAfter.left).toBe(rect.left);

    // Dragging downwards decreases the value again.
    await userEvent.pointer([
      {
        keys: '[MouseLeft>]',
        target: dial,
        coords: { clientX: centerX, clientY: centerY },
      },
      {
        target: dial,
        coords: { clientX: centerX, clientY: centerY + 75 },
      },
      { keys: '[/MouseLeft]' },
    ]);
    await expect(canvas.getByText('Volume: 50')).toBeInTheDocument();
    await expect(args.onChange).toHaveBeenCalledWith(50);
  },
};

export const KeyboardAdjust: Story = {
  render: (args) => renderInteractive(args),
  play: async ({ canvas }) => {
    const dial = canvas.getByRole('slider');
    dial.focus();

    await userEvent.keyboard('{ArrowRight}');
    await expect(canvas.getByText('Volume: 51')).toBeInTheDocument();

    await userEvent.keyboard('{End}');
    await expect(canvas.getByText('Volume: 100')).toBeInTheDocument();

    await userEvent.keyboard('{Home}');
    await expect(canvas.getByText('Volume: 0')).toBeInTheDocument();
  },
};

export const Disabled: Story = {
  args: {
    modelValue: 40,
    label: 'Locked',
    disabled: true,
    onChange: fn(),
  },
  render: (args) => renderInteractive(args),
  play: async ({ args, canvas }) => {
    const dial = canvas.getByRole('slider');
    await expect(dial).toHaveAttribute('aria-valuenow', '40');

    await userEvent.pointer([
      { keys: '[MouseLeft>]', target: dial },
      { keys: '[/MouseLeft]' },
    ]);
    await userEvent.keyboard('{ArrowRight}');

    await expect(canvas.getByText('Locked: 40')).toBeInTheDocument();
    await expect(args.onChange).not.toHaveBeenCalled();
  },
};