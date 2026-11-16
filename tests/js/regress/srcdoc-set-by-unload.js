// An iframe's srcdoc is set again, so the frame starts its new document
// from the attribute's text. Beginning it ends the old document, whose
// unload listener sets the attribute once more, which frees the text the
// frame was about to parse (frame_show_html in frames.c read it after
// au_docview_begin: a use of freed memory, found by Domato seed 108).
try{var f=document.createElement('iframe');f.srcdoc='<script>onunload=function(){frameElement.setAttribute("srcdoc","<p>c</p>")};onpagehide=onunload</script><p>a</p>';document.body.appendChild(f)}catch(_){}
//@@step
try{f.srcdoc='<p>b</p><input>'}catch(_){}
//@@step
try{f.srcdoc='<p>d</p>'}catch(_){}
//@@step
try{f.remove()}catch(_){}
