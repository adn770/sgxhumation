"""Emit probe tables (TOML) with hypotheses; dedupe hypotheses with equal predictions."""
HDR = "# SPDX-License-Identifier: MIT\n# SPDX-FileCopyrightText: 2026 Josep Torra\n"

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'

def v(x):
    if isinstance(x, bool): return 'true' if x else 'false'
    if isinstance(x, int): return f"0x{x & 0xffffffff:08x}" if (x < 0 or x > 9) else str(x)
    if isinstance(x, float): return repr(x)
    if isinstance(x, str): return q(x)
    if isinstance(x, list): return "[" + ", ".join(v(i) for i in x) + "]"
    if isinstance(x, dict): return "{ " + ", ".join(f"{k} = {v(i)}" for k, i in x.items()) + " }"
    raise TypeError(x)

def dedupe(hyps):
    out = {}
    for name, pred in hyps.items():
        key = repr(pred)
        for n2 in list(out):
            if repr(out[n2]) == key:
                out[n2 + "; " + name] = out.pop(n2); break
        else:
            out[name] = pred
    return out

def probe(name, question, source, params, inputs, cases, hyps, labels=None, temps=None):
    hyps = dedupe(hyps)
    s = f"[[probe]]\nname = {q(name)}\nquestion = {q(question)}\nsource = {q(source)}\n"
    if temps: s += f"temps = {temps}\n"
    s += f"params = {v(params)}\ninputs = {v(inputs)}\n"
    if labels: s += f"labels = {v(labels)}\n"
    s += f"cases = {v(cases)}\n"
    if hyps:
        s += "[probe.hypotheses]\n" + "".join(f"{q(n)} = {v(p)}\n" for n, p in hyps.items())
    return s

DUMP0 = {k: 0 for k in "ABCDEFGH"}
def dparams(**kw):
    p = dict(DUMP0); p.update(kw); return p
PRESET = [0xA0A1A2A3 + 0x04040404 * i for i in range(8)]   # r10-r17 before OP
