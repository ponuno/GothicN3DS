# Patches picasso's shared-uniform table so names found again are not
# appended a second time (16 names x 6 files already filled the table of 96,
# so any 17th uniform overflowed it). See docs/plan-reflections.md, round 1.
p = 'picasso/source/picasso_assembler.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
start = s.index('\t\t// If not found, create it')
end = s.index('\n\t\tif (*argText', start)
old = s[start:end]
new = '''		// If not found, create it (and record it exactly once).
		if (uniformPos < 0)
		{
			if (useSharedSpace && g_uniformCount >= MAX_UNIFORM)
				return throwError("too many global uniforms: %s\\n", argText);

			uniformPos = alloc.AllocGlobal(uSize);
			if (uniformPos < 0)
				return throwError("not enough uniform space: %s[%d]\\n", argText, uSize);
			if (useSharedSpace)
				g_uniformTable[g_uniformCount++].init(argText, uniformPos, uSize, dirParam);
		}
'''
if old == new:
    print('already patched')
elif 'g_uniformTable[g_uniformCount++].init(argText, uniformPos, uSize, dirParam);' in old:
    s = s[:start] + new + s[end:]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    raise SystemExit('pattern not found')
