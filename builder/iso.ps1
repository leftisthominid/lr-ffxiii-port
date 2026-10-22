# Lightning Returns builder - reads files out of a PS3 disc image (ISO 9660, with Joliet names
# when present) without mounting it and without other tools. Read-only. Dot-sourced.

if (-not ('LrIso' -as [type])) {
  Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class LrIso {
  const int Sector = 2048;

  static byte[] Read(FileStream fs, long offset, int count) {
    var b = new byte[count];
    fs.Seek(offset, SeekOrigin.Begin);
    int got = 0;
    while (got < count) {
      int n = fs.Read(b, got, count - got);
      if (n <= 0) throw new IOException("Unexpected end of the disc image");
      got += n;
    }
    return b;
  }
  static uint LE32(byte[] b, int o) { return (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); }

  // Root directory (extent, size) and whether names are Joliet (UCS-2 big-endian).
  static void Root(FileStream fs, out uint extent, out uint size, out bool joliet) {
    extent = 0; size = 0; joliet = false;
    bool found = false;
    for (int s = 16; s < 64; ++s) {
      byte[] vd = Read(fs, (long)s * Sector, Sector);
      if (Encoding.ASCII.GetString(vd, 1, 5) != "CD001") throw new InvalidDataException("Not an ISO 9660 disc image");
      if (vd[0] == 255) break;
      bool isJoliet = vd[0] == 2 && vd[88] == 0x25 && vd[89] == 0x2F && (vd[90] == 0x40 || vd[90] == 0x43 || vd[90] == 0x45);
      if (vd[0] == 1 && !found || isJoliet) {
        extent = LE32(vd, 156 + 2); size = LE32(vd, 156 + 10); joliet = isJoliet; found = true;
      }
    }
    if (!found) throw new InvalidDataException("No ISO 9660 volume descriptor");
  }

  static string Name(byte[] rec, int o, int len, bool joliet) {
    string n = joliet ? Encoding.BigEndianUnicode.GetString(rec, o, len) : Encoding.ASCII.GetString(rec, o, len);
    int semi = n.IndexOf(';');
    if (semi >= 0) n = n.Substring(0, semi);
    if (n.EndsWith(".")) n = n.Substring(0, n.Length - 1);
    return n;
  }

  // The pieces of one entry in a directory: {extent, size} pairs, then isDir as the last element;
  // null when it is not there. Large files can be stored in several pieces ("multi-extent":
  // every record but the last has flag 0x80), each its own directory record with the same name.
  static List<uint> Find(FileStream fs, uint dirExtent, uint dirSize, string name, bool joliet) {
    byte[] dir = Read(fs, (long)dirExtent * Sector, (int)dirSize);
    List<uint> found = null;
    int o = 0;
    while (o < dir.Length) {
      int len = dir[o];
      if (len == 0) { o = (o / Sector + 1) * Sector; continue; }
      int nameLen = dir[o + 32];
      int flags = dir[o + 25];
      if (nameLen > 0 && !(nameLen == 1 && (dir[o + 33] == 0 || dir[o + 33] == 1)) &&
          string.Equals(Name(dir, o + 33, nameLen, joliet), name, StringComparison.OrdinalIgnoreCase)) {
        if (found == null) found = new List<uint>();
        found.Add(LE32(dir, o + 2));
        found.Add(LE32(dir, o + 10));
        if ((flags & 0x80) == 0) {
          found.Add((uint)((flags & 2) != 0 ? 1 : 0));
          return found;
        }
      } else if (found != null) {
        break;  // a piece list that ends without its last record
      }
      o += len;
    }
    if (found != null) found.Add(0);
    return found;
  }

  // The pieces of a file ("PS3_GAME/USRDIR/x.bin") as {byte offset, size} pairs, or null.
  public static long[] Parts(string iso, string path) {
    using (var fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read)) {
      uint extent, size; bool joliet;
      Root(fs, out extent, out size, out joliet);
      string[] parts = path.Replace('\\', '/').Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
      for (int i = 0; i < parts.Length; ++i) {
        List<uint> e = Find(fs, extent, size, parts[i], joliet);
        if (e == null) return null;
        bool isDir = e[e.Count - 1] == 1;
        if (i < parts.Length - 1) {
          if (!isDir) return null;
          extent = e[0]; size = e[1];
          continue;
        }
        var r = new long[e.Count - 1];
        for (int k = 0; k + 1 < e.Count; k += 2) { r[k] = (long)e[k] * Sector; r[k + 1] = e[k + 1]; }
        return r;
      }
      return null;
    }
  }

  // Byte offset and size of a file, for reading it in place: {-1, -1} when it is not there,
  // {-2, size} when its pieces are not one after the other on the disc (then Extract it).
  public static long[] Locate(string iso, string path) {
    long[] p = Parts(iso, path);
    if (p == null) return new long[] { -1, -1 };
    long total = 0;
    bool together = true;
    for (int k = 0; k < p.Length; k += 2) {
      total += p[k + 1];
      if (k > 0 && p[k] != p[k - 2] + p[k - 1]) together = false;
    }
    return new long[] { together ? p[0] : -2, total };
  }

  // Copies a file out of the image, piece by piece.
  public static void Extract(string iso, string path, string dest) {
    long[] p = Parts(iso, path);
    if (p == null) throw new FileNotFoundException("Not on the disc: " + path);
    using (var fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
    using (var o = new FileStream(dest, FileMode.Create, FileAccess.Write)) {
      var buf = new byte[1 << 20];
      for (int k = 0; k < p.Length; k += 2) {
        fs.Seek(p[k], SeekOrigin.Begin);
        long left = p[k + 1];
        while (left > 0) {
          int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
          if (n <= 0) throw new IOException("Unexpected end of the disc image");
          o.Write(buf, 0, n);
          left -= n;
        }
      }
    }
  }
}
'@
}

# PS3 disc: an .iso (read directly) or a folder (the disc's files).
function Copy-Ps3File($ps3, [string]$path, [string]$dest) {
  New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
  if ($ps3.kind -eq 'iso') {
    [LrIso]::Extract($ps3.path, $path, $dest)
  } else {
    Copy-Item -LiteralPath (Join-Path $ps3.path ($path -replace '/', '\')) -Destination $dest -Force
  }
}

function Read-Ps3Sfo($ps3) {
  $tmp = Join-Path ([IO.Path]::GetTempPath()) 'lr_builder_param.sfo'
  Copy-Ps3File $ps3 'PS3_GAME/PARAM.SFO' $tmp
  $b = [IO.File]::ReadAllBytes($tmp)
  Remove-Item -LiteralPath $tmp -Force
  return [Text.Encoding]::UTF8.GetString($b)
}
