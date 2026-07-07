import { action } from "./_generated/server";
import { v } from "convex/values";

// Echo the single arg back unchanged.
export const echoAction = action({
  args: { x: v.any() },
  handler: async (_ctx, { x }) => {
    return x;
  },
});

// Return the current wall-clock time (ms since epoch) as a number.
export const now = action({
  args: {},
  handler: async (): Promise<number> => {
    return Date.now();
  },
});
