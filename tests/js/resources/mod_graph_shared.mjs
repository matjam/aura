import { leaf, leafMeta } from './mod_graph_leaf.mjs';
window.__graph.push('shared');
export const shared = 's' + leaf;
export const sharedMeta = import.meta.url;
export { leafMeta };
