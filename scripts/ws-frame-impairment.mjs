function requireNonNegativeInteger(name, value) {
  if (!Number.isInteger(value) || value < 0) {
    throw new Error(`${name} must be a non-negative integer`);
  }
}

export function createDeterministicUnitRandom(seed) {
  requireNonNegativeInteger('seed', seed);
  let state = (seed >>> 0) || 0x9e3779b9;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 0x100000000;
  };
}

export function createFrameImpairment({
  dropEvery = 0,
  duplicateEvery = 0,
  reorderEvery = 0,
  isSelected = () => true,
  onEvent = () => {},
} = {}) {
  requireNonNegativeInteger('dropEvery', dropEvery);
  requireNonNegativeInteger('duplicateEvery', duplicateEvery);
  requireNonNegativeInteger('reorderEvery', reorderEvery);

  let selectedOrdinal = 0;
  let held = null;

  function deliveriesFor(frame, ordinal) {
    const deliveries = [frame];
    if (duplicateEvery > 0 && ordinal % duplicateEvery === 0) {
      deliveries.push(Buffer.from(frame));
      onEvent('duplicate', ordinal);
    }
    return deliveries;
  }

  return {
    push(frame) {
      if (!isSelected(frame)) return [frame];

      selectedOrdinal++;
      const ordinal = selectedOrdinal;
      if (dropEvery > 0 && ordinal % dropEvery === 0) {
        onEvent('drop', ordinal);
        return [];
      }

      if (held) {
        const previous = held;
        held = null;
        onEvent('reorder', previous.ordinal);
        return [
          ...deliveriesFor(frame, ordinal),
          ...deliveriesFor(previous.frame, previous.ordinal),
        ];
      }

      if (reorderEvery > 0 && ordinal % reorderEvery === 0) {
        held = {
          frame: Buffer.from(frame),
          ordinal,
        };
        onEvent('hold', ordinal);
        return [];
      }

      return deliveriesFor(frame, ordinal);
    },

    flush() {
      if (!held) return [];
      const previous = held;
      held = null;
      onEvent('flush', previous.ordinal);
      return deliveriesFor(previous.frame, previous.ordinal);
    },

    selectedCount() {
      return selectedOrdinal;
    },
  };
}
