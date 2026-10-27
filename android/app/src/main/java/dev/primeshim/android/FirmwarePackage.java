package dev.primeshim.android;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/**
 * Reads the official HP Prime update containers without external tools.
 *
 * PRIME_APP.DAT is an APDSK container: a 0x2000-byte header followed by a
 * FAT16 volume (the calculator's A: drive).  Its root contains the system
 * DLLs, APPSLIST.INI/MAP/MD5 and programs/misc/armfir.elf.
 *
 * PRIME_MASTER.DAT is the boot image and has no FAT/ELF; PRIME_OS.ROM is the
 * 1 MiB boot ROM.  PrimeShim starts at the application ELF, so neither is
 * needed for the shim itself.
 *
 * The official A: volume has no dummy.DAT.  programs/misc/armfir.dat from the
 * same volume contains the firmware's fonts/help resources, so it is published
 * as A:\dummy.DAT; without it the calculator boots with no text.
 */
final class FirmwarePackage {

    static final String APP_MAGIC = "APDSKXXX.DAT";
    static final String MASTER_MAGIC = "EA656XXX.DAT";
    static final int APP_VOLUME_OFFSET = 0x2000;

    /** A few files are read in one go; 256 KiB keeps the allocation small. */
    private static final int COPY_BUFFER = 256 * 1024;
    private static final int MAX_DIRECTORY_BYTES = 32 * 1024 * 1024;
    private static final int MAX_DEPTH = 32;

    interface Listener {
        void onEntry(String path);
    }

    static final class Result {
        int files;
        long bytes;
        boolean dummyCreated;
        int manifestFiles;
        int manifestMismatches;
        String manifestProblem;
    }

    private FirmwarePackage() {
    }

    /* ---- container sniffing ------------------------------------------ */

    static boolean isAppContainer(File file) {
        return hasMagic(file, APP_MAGIC);
    }

    static boolean isMasterContainer(File file) {
        return hasMagic(file, MASTER_MAGIC);
    }

    static boolean isElf(File file) {
        if (file == null || !file.isFile() || file.length() < 52) return false;
        try {
            RandomAccessFile raf = new RandomAccessFile(file, "r");
            try {
                byte[] header = new byte[52];
                raf.readFully(header);
                if (header[0] != 0x7f || header[1] != 'E'
                        || header[2] != 'L' || header[3] != 'F') {
                    return false;
                }
                int type = u16(header, 16);
                int machine = u16(header, 18);
                return type == 2 && machine == 40;      /* ET_EXEC, EM_ARM */
            } finally {
                raf.close();
            }
        } catch (IOException error) {
            return false;
        }
    }

    static boolean looksLikeOsRom(File file) {
        return file != null && file.isFile() && file.length() == 1024 * 1024;
    }

    private static boolean hasMagic(File file, String magic) {
        if (file == null || !file.isFile() || file.length() < magic.length()) {
            return false;
        }
        try {
            InputStream in = new BufferedInputStream(new FileInputStream(file), 64);
            try {
                byte[] header = new byte[magic.length()];
                int read = 0;
                while (read < header.length) {
                    int count = in.read(header, read, header.length - read);
                    if (count <= 0) return false;
                    read += count;
                }
                return magic.equals(new String(header, StandardCharsets.US_ASCII));
            } finally {
                in.close();
            }
        } catch (IOException error) {
            return false;
        }
    }

    /* ---- A: extraction ----------------------------------------------- */

    /**
     * Unpacks the FAT16 A: volume inside {@code container} below
     * {@code destinationRoot}.  Existing files are overwritten; the caller is
     * expected to pass an empty staging directory.
     */
    static Result extractAppDisk(File container, File destinationRoot,
                                 Listener listener) throws IOException {
        Result result = new Result();
        if (!isAppContainer(container)) {
            throw new IOException(L.t("不是 PRIME_APP.DAT 容器"));
        }
        if (!destinationRoot.isDirectory() && !destinationRoot.mkdirs()) {
            throw new IOException(L.t("无法创建 ") + destinationRoot);
        }

        RandomAccessFile raf = new RandomAccessFile(container, "r");
        try {
            Fat fat = Fat.open(raf, APP_VOLUME_OFFSET);
            byte[] root = new byte[fat.rootEntries * 32];
            fat.readAt(fat.rootOffset, root);
            extractDirectory(raf, fat, root, destinationRoot, result, listener, 0);

            // The official A: volume has no dummy.DAT, but the firmware reads
            // its font/help database from that file.  programs/misc/armfir.dat
            // is the same generation and contains the Prime Sans faces, so
            // publish it as A:\dummy.DAT (hard link when possible).
            result.dummyCreated = installDummy(destinationRoot);
            verifyManifest(destinationRoot, result);
            return result;
        } finally {
            raf.close();
        }
    }

    private static void extractDirectory(RandomAccessFile raf, Fat fat,
                                         byte[] directory, File parent,
                                         Result result, Listener listener,
                                         int depth) throws IOException {
        if (depth > MAX_DEPTH) {
            throw new IOException(L.t("目录层级过深"));
        }

        List<LfnPart> lfn = new ArrayList<>();
        int entries = directory.length / 32;
        for (int i = 0; i < entries; ++i) {
            int offset = i * 32;
            int first = directory[offset] & 0xFF;
            if (first == 0x00) break;                 /* end of directory */
            if (first == 0xE5) {                      /* deleted entry */
                lfn.clear();
                continue;
            }

            int attr = directory[offset + 11] & 0xFF;
            if (attr == 0x0F) {                       /* VFAT long name */
                lfn.add(LfnPart.parse(directory, offset));
                continue;
            }

            String shortName = shortName(directory, offset);
            if (shortName.equals(".") || shortName.equals("..")) {
                lfn.clear();
                continue;
            }
            if ((attr & 0x08) != 0) {                 /* volume label */
                lfn.clear();
                continue;
            }

            String name = assembleLfn(lfn, directory, offset);
            lfn.clear();
            if (name == null || name.isEmpty()) name = shortName;
            name = safeName(name);
            if (name.isEmpty()) continue;

            int cluster = u16(directory, offset + 26);
            int high = u16(directory, offset + 20);
            if (high != 0) cluster |= high << 16;
            long size = u32(directory, offset + 28);

            File target = new File(parent, name);
            if ((attr & 0x10) != 0) {                 /* directory */
                if (cluster < 2) continue;            /* empty or invalid */
                if (!target.isDirectory() && !target.mkdirs()) {
                    throw new IOException(L.t("无法创建目录 ") + target);
                }
                byte[] child = readChain(raf, fat, cluster);
                extractDirectory(raf, fat, child, target, result, listener,
                        depth + 1);
            } else {
                File parentFile = target.getParentFile();
                if (parentFile != null && !parentFile.isDirectory()
                        && !parentFile.mkdirs()) {
                    throw new IOException(L.t("无法创建目录 ") + parentFile);
                }
                long written = writeFile(raf, fat, cluster, size, target);
                result.files++;
                result.bytes += written;
                if (listener != null) listener.onEntry(name);
            }
        }
    }

    private static long writeFile(RandomAccessFile raf, Fat fat, int cluster,
                                  long size, File target) throws IOException {
        OutputStream out = new BufferedOutputStream(
                new FileOutputStream(target), COPY_BUFFER);
        byte[] buffer = new byte[COPY_BUFFER];
        long remaining = size;
        int current = cluster;
        int guard = 0;
        try {
            if (remaining == 0) return 0;
            if (current < 2) throw new IOException(L.t("文件没有数据簇: ") + target);
            while (remaining > 0) {
                if (current < 2 || current >= fat.maxCluster) {
                    throw new IOException(L.t("FAT 链提前结束: ") + target);
                }
                long offset = fat.clusterOffset(current);
                int chunk = (int) Math.min(remaining, fat.clusterBytes);
                fat.readAt(offset, buffer, chunk);
                out.write(buffer, 0, chunk);
                remaining -= chunk;
                current = fat.next(current);
                if (++guard > fat.maxCluster + 2) {
                    throw new IOException(L.t("FAT 链成环: ") + target);
                }
            }
            out.flush();
            return size;
        } finally {
            out.close();
        }
    }

    private static byte[] readChain(RandomAccessFile raf, Fat fat, int cluster)
            throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[fat.clusterBytes];
        int current = cluster;
        int guard = 0;
        while (current >= 2 && current < fat.maxCluster) {
            fat.readAt(fat.clusterOffset(current), buffer, buffer.length);
            out.write(buffer, 0, buffer.length);
            if (out.size() > MAX_DIRECTORY_BYTES) {
                throw new IOException(L.t("目录过大"));
            }
            current = fat.next(current);
            if (++guard > fat.maxCluster + 2) {
                throw new IOException(L.t("FAT 目录链成环"));
            }
        }
        return out.toByteArray();
    }

    /* ---- names ------------------------------------------------------- */

    private static final class LfnPart {
        final int sequence;
        final int checksum;
        final String text;

        private LfnPart(int sequence, int checksum, String text) {
            this.sequence = sequence;
            this.checksum = checksum;
            this.text = text;
        }

        static LfnPart parse(byte[] entry, int offset) {
            int sequence = entry[offset] & 0x1F;
            int checksum = entry[offset + 13] & 0xFF;
            StringBuilder text = new StringBuilder(13);
            append(text, entry, offset + 1, 5);
            append(text, entry, offset + 14, 6);
            append(text, entry, offset + 28, 2);
            return new LfnPart(sequence, checksum, text.toString());
        }

        private static void append(StringBuilder text, byte[] entry,
                                   int offset, int chars) {
            for (int i = 0; i < chars; ++i) {
                int low = entry[offset + i * 2] & 0xFF;
                int high = entry[offset + i * 2 + 1] & 0xFF;
                int value = low | (high << 8);
                if (value == 0x0000 || value == 0xFFFF) continue;
                text.append((char) value);
            }
        }
    }

    /** Returns the long name only when every entry checksum matches. */
    private static String assembleLfn(List<LfnPart> parts, byte[] entry,
                                      int offset) {
        if (parts.isEmpty()) return null;
        int expected = shortNameChecksum(entry, offset);
        for (LfnPart part : parts) {
            if (part.checksum != expected) return null;
        }
        List<LfnPart> ordered = new ArrayList<>(parts);
        ordered.sort(Comparator.comparingInt(part -> part.sequence));
        StringBuilder name = new StringBuilder();
        for (LfnPart part : ordered) name.append(part.text);
        return name.toString();
    }

    private static int shortNameChecksum(byte[] entry, int offset) {
        int sum = 0;
        for (int i = 0; i < 11; ++i) {
            sum = (((sum & 1) << 7) + (sum >> 1)
                    + (entry[offset + i] & 0xFF)) & 0xFF;
        }
        return sum;
    }

    private static String shortName(byte[] entry, int offset) {
        StringBuilder base = new StringBuilder(8);
        for (int i = 0; i < 8; ++i) {
            int value = entry[offset + i] & 0xFF;
            if (value == 0x00 || value == 0x20) break;
            base.append((char) (value == 0x05 ? 0xE5 : value));
        }
        StringBuilder extension = new StringBuilder(3);
        for (int i = 8; i < 11; ++i) {
            int value = entry[offset + i] & 0xFF;
            if (value == 0x00 || value == 0x20) break;
            extension.append((char) value);
        }
        int flags = entry[offset + 12] & 0xFF;
        String name = (flags & 0x08) != 0
                ? base.toString().toLowerCase(Locale.US) : base.toString();
        String ext = (flags & 0x10) != 0
                ? extension.toString().toLowerCase(Locale.US) : extension.toString();
        return ext.isEmpty() ? name : name + "." + ext;
    }

    private static String safeName(String name) {
        StringBuilder safe = new StringBuilder(name.length());
        for (int i = 0; i < name.length(); ++i) {
            char c = name.charAt(i);
            if (c == '/' || c == '\\' || c == '\0' || c < 0x20) {
                safe.append('_');
            } else {
                safe.append(c);
            }
        }
        String value = safe.toString();
        if (value.equals(".") || value.equals("..")) return "";
        if (value.length() > 200) value = value.substring(0, 200);
        return value;
    }

    /* ---- dummy / manifest ------------------------------------------- */

    /**
     * Publishes A:\programs\misc\armfir.dat as A:\dummy.DAT.  The firmware
     * reads its built-in fonts and help resources from this database; without
     * it the calculator boots but draws no text.  A hard link avoids a second
     * ~10 MiB copy on Android's ext4 private storage.
     */
    static boolean installDummy(File aRoot) throws IOException {
        File dummy = new File(aRoot, "dummy.DAT");
        if (dummy.isFile() && dummy.length() > 0) return false;
        File source = findIgnoreCase(aRoot, "programs/misc/armfir.dat");
        if (source == null) source = findIgnoreCase(aRoot, "armfir.dat");
        if (source == null) return false;
        try {
            java.nio.file.Files.createLink(dummy.toPath(), source.toPath());
            return true;
        } catch (Exception ignored) {
            /* filesystem without hard links: fall through to a copy */
        }
        copyFile(source, dummy);
        return true;
    }

    private static void verifyManifest(File aRoot, Result result) {
        File manifest = findIgnoreCase(aRoot, "APPSLIST.MD5");
        if (manifest == null) return;
        List<String> lines;
        try {
            lines = readLines(manifest);
        } catch (IOException error) {
            result.manifestProblem = error.getMessage();
            return;
        }
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("MD5");
        } catch (Exception error) {
            result.manifestProblem = error.toString();
            return;
        }
        byte[] buffer = new byte[COPY_BUFFER];
        for (String line : lines) {
            line = line.trim();
            if (line.isEmpty()) continue;
            int space = -1;
            for (int i = 0; i < line.length(); ++i) {
                if (Character.isWhitespace(line.charAt(i))) {
                    space = i;
                    break;
                }
            }
            if (space <= 0) continue;
            String expected = line.substring(0, space);
            String path = line.substring(space).trim();
            while (path.startsWith("\\") || path.startsWith("/")) {
                path = path.substring(1);
            }
            if (!expected.matches("[0-9a-fA-F]{32}")) continue;
            File file = findIgnoreCase(aRoot, path);
            result.manifestFiles++;
            if (file == null || !file.isFile()) {
                result.manifestMismatches++;
                continue;
            }
            try {
                digest.reset();
                InputStream in = new BufferedInputStream(
                        new FileInputStream(file), COPY_BUFFER);
                try {
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        digest.update(buffer, 0, read);
                    }
                } finally {
                    in.close();
                }
                String actual = hex(digest.digest());
                if (!actual.equalsIgnoreCase(expected)) {
                    result.manifestMismatches++;
                }
            } catch (IOException error) {
                result.manifestMismatches++;
            }
        }
    }

    /** Finds a relative path, matching each component case-insensitively. */
    private static File findIgnoreCase(File root, String relative) {
        if (relative == null) return null;
        String[] parts = relative.replace('\\', '/').split("/");
        File current = root;
        for (String part : parts) {
            if (part.isEmpty()) continue;
            File exact = new File(current, part);
            if (exact.exists()) {
                current = exact;
                continue;
            }
            File[] children = current.listFiles();
            if (children == null) return null;
            File match = null;
            for (File child : children) {
                if (child.getName().equalsIgnoreCase(part)) {
                    match = child;
                    break;
                }
            }
            if (match == null) return null;
            current = match;
        }
        return current;
    }

    private static List<String> readLines(File file) throws IOException {
        List<String> lines = new ArrayList<>();
        InputStream in = new BufferedInputStream(new FileInputStream(file));
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        try {
            int value;
            while ((value = in.read()) >= 0) {
                if (value == '\n') {
                    lines.add(new String(line.toByteArray(), StandardCharsets.UTF_8));
                    line.reset();
                } else if (value != '\r') {
                    line.write(value);
                }
            }
            if (line.size() > 0) {
                lines.add(new String(line.toByteArray(), StandardCharsets.UTF_8));
            }
        } finally {
            in.close();
        }
        return lines;
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            text.append(String.format(Locale.US, "%02x", value & 0xFF));
        }
        return text.toString();
    }

    static void copyFile(File source, File target) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException(L.t("无法创建目录 ") + parent);
        }
        InputStream in = new BufferedInputStream(
                new FileInputStream(source), COPY_BUFFER);
        OutputStream out = new BufferedOutputStream(
                new FileOutputStream(target), COPY_BUFFER);
        try {
            byte[] buffer = new byte[COPY_BUFFER];
            int read;
            while ((read = in.read(buffer)) > 0) out.write(buffer, 0, read);
        } finally {
            try {
                out.close();
            } finally {
                in.close();
            }
        }
    }

    /* ---- FAT16 ------------------------------------------------------- */

    private static final class Fat {
        final RandomAccessFile file;
        final byte[] table;
        final int clusterBytes;
        final int rootEntries;
        final long rootOffset;
        final long dataOffset;
        final int maxCluster;

        private Fat(RandomAccessFile file, byte[] table, int clusterBytes,
                    int rootEntries, long rootOffset, long dataOffset,
                    int maxCluster) {
            this.file = file;
            this.table = table;
            this.clusterBytes = clusterBytes;
            this.rootEntries = rootEntries;
            this.rootOffset = rootOffset;
            this.dataOffset = dataOffset;
            this.maxCluster = maxCluster;
        }

        static Fat open(RandomAccessFile file, long volumeOffset)
                throws IOException {
            byte[] boot = new byte[512];
            file.seek(volumeOffset);
            file.readFully(boot);
            int bytesPerSector = u16(boot, 11);
            int sectorsPerCluster = boot[13] & 0xFF;
            int reserved = u16(boot, 14);
            int fats = boot[16] & 0xFF;
            int rootEntries = u16(boot, 17);
            long totalSectors = u16(boot, 19);
            long fatSectors = u16(boot, 22);
            if (totalSectors == 0) totalSectors = u32(boot, 32);
            if (fatSectors == 0) {
                throw new IOException(L.t("不是 FAT16 卷，FAT32 不支持"));
            }
            if (bytesPerSector != 512 || sectorsPerCluster < 1
                    || sectorsPerCluster > 128 || reserved < 1 || fats < 1
                    || rootEntries < 1 || totalSectors <= 0) {
                throw new IOException(L.t("FAT16 引导扇区无效"));
            }
            long rootDirSectors = (rootEntries * 32L + bytesPerSector - 1)
                    / bytesPerSector;
            long firstDataSector = reserved + (long) fats * fatSectors
                    + rootDirSectors;
            long dataSectors = totalSectors - firstDataSector;
            if (dataSectors <= 0) throw new IOException(L.t("FAT16 数据区为空"));
            long clusters = dataSectors / sectorsPerCluster;
            if (clusters < 4085) {
                throw new IOException(L.t("卷是 FAT12，不是 FAT16"));
            }
            if (clusters > 65524) {
                throw new IOException(L.t("卷是 FAT32，不是 FAT16"));
            }
            int clusterBytes = bytesPerSector * sectorsPerCluster;
            int fatBytes = (int) (fatSectors * bytesPerSector);
            byte[] table = new byte[fatBytes];
            file.seek(volumeOffset + (long) reserved * bytesPerSector);
            file.readFully(table);
            long rootOffset = volumeOffset + (reserved + (long) fats * fatSectors)
                    * bytesPerSector;
            long dataOffset = volumeOffset + firstDataSector * bytesPerSector;
            return new Fat(file, table, clusterBytes, rootEntries, rootOffset,
                    dataOffset, (int) clusters + 1);
        }

        int next(int cluster) {
            int offset = cluster * 2;
            int value = u16(table, offset);
            if (value >= 0xFFF8) return 0xFFFF;      /* end of chain */
            if (value == 0xFFF7) return 0xFFFF;      /* bad cluster */
            if (value >= 0xFFF0) return 0xFFFF;      /* reserved */
            return value;
        }

        long clusterOffset(int cluster) {
            return dataOffset + (long) (cluster - 2) * clusterBytes;
        }

        void readAt(long offset, byte[] buffer) throws IOException {
            readAt(offset, buffer, buffer.length);
        }

        void readAt(long offset, byte[] buffer, int length) throws IOException {
            file.seek(offset);
            file.readFully(buffer, 0, length);
        }
    }

    private static int u16(byte[] bytes, int offset) {
        return (bytes[offset] & 0xFF) | ((bytes[offset + 1] & 0xFF) << 8);
    }

    private static long u32(byte[] bytes, int offset) {
        return (bytes[offset] & 0xFFL)
                | ((bytes[offset + 1] & 0xFFL) << 8)
                | ((bytes[offset + 2] & 0xFFL) << 16)
                | ((bytes[offset + 3] & 0xFFL) << 24);
    }
}
