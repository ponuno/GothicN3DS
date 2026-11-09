# Patches picasso's shared-uniform table so names found again are not
# appended a second time (16 names x 6 files already filled the table of 96,
# so any 17th uniform overflowed it). See docs/plan-reflections.md, round 1.
p = 'picasso/source/picasso_assembler.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
old = '''		if (useSharedSpace)
			g_uniformTable[g_uniformCount++].init(argText, uniformPos, uSize, dirParam);'''
new = '''		if (useSharedSpace)
		{
			if (g_uniformCount == MAX_UNIFORM)
				return throwError("too many global uniforms: %s\\n", argText);
			g_uniformTable[g_uniformCount++].init(argText, uniformPos, uSize, dirParam);
		}'''
if old in s:
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
elif new in s:
    print('already patched')
else:
    raise SystemExit('pattern not found')
