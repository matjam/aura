// A graph with shared imports: the root imports four modules that all
// import mod_graph_shared.mjs, which imports mod_graph_leaf.mjs.
import { d1 } from './mod_graph_d1.mjs';
import { d2 } from './mod_graph_d2.mjs';
import { d3 } from './mod_graph_d3.mjs';
import { d4 } from './mod_graph_d4.mjs';
window.__graph.push('root');
window.__graphResult = [d1, d2, d3, d4].join(',');
window.__graphMeta = import.meta.url;
