import { shared, sharedMeta, leafMeta } from './mod_graph_shared.mjs';
window.__graph.push('d1');
window.__graphMetas = [import.meta.url, sharedMeta, leafMeta];
export const d1 = '1' + shared;
