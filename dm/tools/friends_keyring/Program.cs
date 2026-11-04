// Read only a privately supplied, decrypted friends NSO; publish no firmware or key bytes.
using System;
using System.IO;
using System.Security.Cryptography;
class Program {
    static int Main(string[] args) {
        if (args.Length != 2) { Console.Error.WriteLine("Usage: friends_keyring <decrypted-friends.nso> <new-keyring-file>"); return 2; }
        byte[] input = null, keys = null;
        try {
            var info = new FileInfo(args[0]);
            if (!info.Exists || info.Length < 256 || info.Length > 64 * 1024 * 1024) throw new InvalidDataException();
            input = File.ReadAllBytes(args[0]); keys = FriendsPlayHistoryResources.Extract(input);
            var options = new FileStreamOptions { Mode = FileMode.CreateNew, Access = FileAccess.Write, Share = FileShare.None };
            if (!OperatingSystem.IsWindows()) options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
            using (var output = new FileStream(args[1], options)) { output.Write(keys); output.Flush(true); }
            Console.WriteLine("Verified local play-history resource written. Keep this file private."); return 0;
        } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is CryptographicException) {
            Console.Error.WriteLine("Resource unavailable, unsupported or output already exists. No key values are logged."); return 1;
        } finally { if (input != null) CryptographicOperations.ZeroMemory(input); if (keys != null) CryptographicOperations.ZeroMemory(keys); }
    }
}
