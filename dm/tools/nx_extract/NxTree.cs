// NX supplies immutable random-access NCA storage. LibHac retains the existing
// NCA hash-tree, filesystem and BKTR implementation; no NCA payload is unpacked.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using LibHac;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Ncm;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using LibHac.Tools.Ncm;

static partial class P {
    sealed class NxBridge : IDisposable {
        readonly Process process;
        readonly Stream output;
        readonly System.Threading.Tasks.Task<string> errors;
        public readonly Dictionary<int, List<(string Name, uint Id, long Size)>> Files = new();
        public readonly Dictionary<int, string[]> Snapshots = new();
        public readonly Dictionary<int, ulong> Apps = new();
        public NxBridge(string reader, string path) {
            var start = new ProcessStartInfo(reader) { RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            start.ArgumentList.Add(path); process = Process.Start(start); output = process.StandardOutput.BaseStream;
            errors = process.StandardError.ReadToEndAsync();
            try {
                for (;;) {
                    string line = Line(); if (line == "END") break;
                    string[] r = line.Split(' ');
                    if (r[0] == "A") Apps.Add(int.Parse(r[1]), ulong.Parse(r[2]));
                    if (r[0] == "S") { int i = int.Parse(r[2]); Snapshots.Add(i, r); Files.Add(i, new()); }
                    if (r[0] == "F") Files[int.Parse(r[1])].Add((Encoding.UTF8.GetString(Convert.FromHexString(r[2])), uint.Parse(r[3]), long.Parse(r[4])));
                }
            } catch { Dispose(); throw; }
        }
        string Line() {
            var line = new StringBuilder();
            for (;;) {
                int b = output.ReadByte();
                if (b < 0) throw new IOException("NX reader stopped: " + errors.GetAwaiter().GetResult());
                if (b == 10) return line.ToString();
                if (line.Length >= 32768) throw new IOException("NX reader response too long");
                line.Append((char)b);
            }
        }
        public void Read(uint id, long offset, Span<byte> data) {
            lock (process) {
                for (int at=0;at<data.Length;) {
                    int n=Math.Min(4<<20,data.Length-at);
                    process.StandardInput.WriteLine(FormattableString.Invariant($"F {id} {offset+at} {n}")); process.StandardInput.Flush();
                    if (int.Parse(Line(), CultureInfo.InvariantCulture)!=n) throw new IOException("short NX read");
                    output.ReadExactly(data.Slice(at,n)); at+=n;
                }
            }
        }
        public (List<Nca> Ncas, List<string> Ids) Open(int snapshot) {
            var ncas = new List<Nca>(); var ids = new List<string>();
            foreach (var file in Files[snapshot]) if (file.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)) {
                var storage=new NxNcaStorage(this,file.Id,file.Size);
                ncas.Add(new Nca(KeySet.CreateDefaultKeySet(),storage));
                string name=System.IO.Path.GetFileName(file.Name); ids.Add(name.Split('.')[0].ToLowerInvariant());
            }
            if (ncas.Count==0) throw new IOException("NX snapshot has no native NCAs");
            return (ncas,ids);
        }
        public void Dispose() {
            process.StandardInput.Close();
            if (!process.WaitForExit(1000)) { process.Kill(); process.WaitForExit(); }
            process.StandardOutput.Dispose(); process.Dispose();
        }
    }
    sealed class NxNcaStorage : IStorage {
        readonly NxBridge source; readonly uint id; readonly long size;
        public NxNcaStorage(NxBridge source,uint id,long size) {
            this.source=source; this.id=id; this.size=size;
            byte[] header=new byte[0xc00]; source.Read(id,0,header);
            if (!header.AsSpan(0x200,4).SequenceEqual("DNCA"u8) || BitConverter.ToInt64(header,0x208)!=size) throw new IOException("invalid decrypted NX NCA header");
            for (int i=0;i<4;i++) if (BitConverter.ToUInt32(header,0x240+i*16)!=0 && header[0x404+i*0x200]!=1)
                throw new IOException("NX NCA section still declares encryption");
        }
        public override Result Read(long offset, Span<byte> destination) {
            var result=CheckAccessRange(offset,destination.Length,size); if (result.IsFailure()) return result;
            source.Read(id,offset,destination);
            // DNCA explicitly promises plaintext. Present the NCA3 header layout
            // to LibHac without changing any stored bytes or section headers.
            ReadOnlySpan<byte> magic="NCA3"u8;
            for (int i=0;i<4;i++) if (offset<=0x200+i && 0x200+i-offset<destination.Length) destination[(int)(0x200+i-offset)]=magic[i];
            return Result.Success;
        }
        public override Result GetSize(out long value) { value=size; return Result.Success; }
        public override Result Flush() => Result.Success;
        public override Result Write(long offset, ReadOnlySpan<byte> data) => ResultFs.UnsupportedOperation.Value;
        public override Result SetSize(long value) => ResultFs.UnsupportedOperation.Value;
        public override Result OperateRange(Span<byte> output, OperationId operation, long offset, long size, ReadOnlySpan<byte> input) {
            output.Clear(); return Result.Success;
        }
    }
    static (List<Nca> Ncas,Cnmt Meta) NxPackage(NxBridge bridge,int snapshot) {
        var (all,ids)=bridge.Open(snapshot); var row=bridge.Snapshots[snapshot];
        int kind=int.Parse(row[3]); uint version=uint.Parse(row[5]); ulong app=bridge.Apps[int.Parse(row[1])];
        var type=kind==0?ContentMetaType.Application:(kind==1 || kind==3)?ContentMetaType.Patch:ContentMetaType.AddOnContent;
        string content=row[10]; Cnmt selected=null;
        var metadata=new List<Nca>();
        for (int i=0;i<all.Count;i++) if (all[i].Header.ContentType==NcaContentType.Meta && ids[i].Equals(content,StringComparison.OrdinalIgnoreCase)) metadata.Add(all[i]);
        foreach (var meta in Metas(metadata)) {
            if (meta.Type!=type || meta.TitleVersion.Version!=version || meta.ApplicationTitleId!=app) continue;
            if (selected!=null) throw new IOException("ambiguous native NX content meta");
            selected=meta;
        }
        if (selected==null) throw new IOException("NX snapshot has no matching native content meta");
        var wanted=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // A patch's meta also lists the delta fragments that update an older version in place; installed
        // content and cartridge dumps do not carry them, and nothing reads them here.
        int required=0;
        foreach (var entry in selected.ContentEntries) {
            if (entry.Type==LibHac.Ncm.ContentType.DeltaFragment) continue;
            wanted.Add(Convert.ToHexString(entry.NcaId)); required++;
        }
        var kept=new List<Nca>();
        for (int i=0;i<all.Count;i++) if (wanted.Contains(ids[i])) kept.Add(all[i]);
        if (kept.Count!=required) throw new IOException("NX snapshot is missing native content records");
        return (kept,selected);
    }
    static int NxTree(string[] args) {
        if (args.Length!=6) throw new IOException("usage: --nx-tree reader archive snapshot base-snapshot-or--1 program-id");
        using var bridge=new NxBridge(args[1],args[2]);
        int index=int.Parse(args[3]), baseIndex=int.Parse(args[4]); ulong pid=ulong.Parse(args[5],NumberStyles.HexNumber,CultureInfo.InvariantCulture);
        var (selected,meta)=NxPackage(bridge,index);
        List<Nca> bases=baseIndex<0?null:NxPackage(bridge,baseIndex).Ncas;
        var paths=new List<(IFileSystem Fs,string Path,long Size)>();
        var owned=new List<IFileSystem>();
        void Tree(string prefix,IFileSystem fs) {
            Console.WriteLine("D " + Convert.ToHexString(Encoding.UTF8.GetBytes(prefix))); owned.Add(fs);
            var entries=new List<DirectoryEntryEx>(fs.EnumerateEntries("*",SearchOptions.RecurseSubdirectories));
            entries.Sort((a,b)=>StringComparer.Ordinal.Compare(a.FullPath,b.FullPath));
            foreach (var entry in entries) {
                string path=prefix+"/"+entry.FullPath.Trim('/');
                if (entry.Type==DirectoryEntryType.Directory) Console.WriteLine("D "+Convert.ToHexString(Encoding.UTF8.GetBytes(path)));
                else { Console.WriteLine($"F {Convert.ToHexString(Encoding.UTF8.GetBytes(path))} {paths.Count} {entry.Size}"); paths.Add((fs,entry.FullPath,entry.Size)); }
            }
        }
        try {
            if (meta.Type==ContentMetaType.AddOnContent) {
                Nca data=Pick(selected,NcaContentType.PublicData,meta.TitleId)??Pick(selected,NcaContentType.Data,meta.TitleId)??throw new IOException("NX DLC has no data NCA");
                Console.WriteLine($"T {meta.TitleId} {meta.MinimumApplicationVersion.Version}");
                Tree("romfs",new RomFsFileSystem(data.OpenStorage(NcaSectionType.Data,IntegrityCheckLevel.ErrorOnInvalid)));
            } else {
                Nca program=Pick(selected,NcaContentType.Program,pid);
                Nca parent=bases==null?null:Pick(bases,NcaContentType.Program,pid);
                if (program==null) { program=parent; parent=null; }
                if (program==null) throw new IOException("NX has no selected Program NCA");
                Tree("exefs",parent==null?program.OpenFileSystem(NcaSectionType.Code,IntegrityCheckLevel.ErrorOnInvalid):parent.OpenFileSystemWithPatch(program,NcaSectionType.Code,IntegrityCheckLevel.ErrorOnInvalid));
                if (program.SectionExists(1) || parent!=null && parent.SectionExists(1))
                    Tree("romfs",new RomFsFileSystem(parent==null?program.OpenStorage(NcaSectionType.Data,IntegrityCheckLevel.ErrorOnInvalid):parent.OpenStorageWithPatch(program,NcaSectionType.Data,IntegrityCheckLevel.ErrorOnInvalid)));
                else Console.WriteLine("D 726f6d6673");
                Nca control=Pick(selected,NcaContentType.Control,pid)??Pick(bases,NcaContentType.Control,pid);
                if (control!=null) Tree("control",control.OpenFileSystem(NcaSectionType.Data,IntegrityCheckLevel.ErrorOnInvalid));
            }
            Console.WriteLine("END"); Console.Out.Flush();
            var input=Console.In; using var output=Console.OpenStandardOutput();
            for (string line;(line=input.ReadLine())!=null;) {
                string[] request=line.Split(' ');
                if (request.Length!=4 || request[0]!="F") throw new IOException("invalid NX file request");
                int id=int.Parse(request[1]), length=int.Parse(request[3]); long offset=long.Parse(request[2]);
                if (id<0 || id>=paths.Count || length<0 || length>4<<20 || offset<0 || offset>paths[id].Size || length>paths[id].Size-offset) throw new IOException("NX file request out of bounds");
                byte[] data=new byte[length]; using var file=new UniqueRef<IFile>();
                paths[id].Fs.OpenFile(ref file.Ref,paths[id].Path.ToU8Span(),OpenMode.Read).ThrowIfFailure();
                file.Get.Read(out long read,offset,data,ReadOption.None).ThrowIfFailure();
                if (read!=length) throw new IOException("short native NX file read");
                Console.WriteLine(length); Console.Out.Flush(); output.Write(data); output.Flush();
            }
            return 0;
        } finally { foreach (var fs in owned) fs.Dispose(); }
    }
}
