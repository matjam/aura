// Imports a module that loads and one with a syntax error.
import { leaf } from './mod_graph_leaf.mjs';
import { x } from './mod_syntax.mjs';
window.__badRan = leaf + x;
