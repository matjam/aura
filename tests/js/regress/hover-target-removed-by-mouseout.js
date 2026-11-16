// The pointer moves from one element to another: the view fires mouseout
// at the old one and then mouseover at the new one. A mouseout listener
// that empties its target frees the new element (a child of the old one)
// once the layout is built again, and au_docscript_hover then read it for
// the mouseover: a use of a freed element, found by fuzz_dom once its
// listeners were put on input events. Reduced by tools/fuzz-reduce.py;
// the seed beside this file gives the pointer positions.
//
try{E.splice(4%(E.h))().e((1))}catch(_){}try{(1)}catch(_){}try{var h=function(ev){try{ev.target.textContent=""}catch(_){}try{(document.y||document.documentElement).offsetWidth}catch(_){}};L.push();document.addEventListener('mouseout',h,true)}catch(_){}try{((4).x).d((6))}catch(_){}try{if(h){((([''])));e=function(){try{(3).e((3)).h}catch(_){}}.e()}}catch(_){}try{(function(){try{((2)())}catch(_){}})}catch(_){}try{t=''}catch(_){}