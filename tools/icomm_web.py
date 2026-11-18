"""Tiny iComm 2.38 web API reader (port 8080): python icomm_web.py DEV BLOCKID...

BLOCKID is the web page's block id (= block sequence number - 1).
Set ICOMM_HOST and ICOMM_PASSWORD (the iComm Control Centre web password).
"""
import http.cookiejar, json, os, re, sys, urllib.parse, urllib.request
BASE = "http://%s:8080" % os.environ.get("ICOMM_HOST", "192.168.1.10")
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
def get(p):
    with op.open(BASE + p, timeout=30) as r:
        return r.read().decode("utf-8", "replace")
def login():
    tok = re.search(r'name="__RequestVerificationToken"[^>]*value="([^"]+)"', get("/iComm/")).group(1)
    op.open(BASE + "/iComm/", data=urllib.parse.urlencode({"__RequestVerificationToken": tok, "Password": os.environ.get("ICOMM_PASSWORD", "")}).encode(), timeout=20).read()
def points(dev, blk):
    return {p["Name"]: p["PointValue"] for p in json.loads(get(f"/iComm/Connection/GetPointsGridData?connectionId=1&deviceId={dev}&blockId={blk}"))}
if __name__ == "__main__":
    login()
    for b in sys.argv[2:]:
        print(b, points(sys.argv[1], b))
