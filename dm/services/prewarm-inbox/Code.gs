const folderProperty = "PREWARM_FOLDER_ID";
const mime = "text/tab-separated-values";
const maxBytes = 32 * 1024 * 1024;
const row = /^(?:S\t[0-9a-f]{16}_b\t[0-7]|C\t[0-9a-f]{16}_b|G\t[0-9a-f]{16}\t[0-9a-f]{16}|(?:R\t|M\t(?:-|[0-9a-f]{16}_b)\t)(?:[0-9a-f]{864}|[0-9a-f]{880}|[0-9a-f]{1008}|[0-9a-f]{1024}))$/;

// Run once as the maintainer. The new folder inherits the owner's private My Drive access.
function setupInbox() {
    const properties = PropertiesService.getScriptProperties();
    let folder = properties.getProperty(folderProperty);
    if (!folder) {
        folder = Drive.Files.create({ name: "nxnative prewarm inbox", mimeType: "application/vnd.google-apps.folder" }).id;
        properties.setProperty(folderProperty, folder);
    }
    console.log("https://drive.google.com/drive/folders/" + folder);
}

function validRows(text) {
    let start = 0;
    while (start < text.length) {
        const end = text.indexOf("\n", start);
        if (end < 0 || end - start > 1045 || !row.test(text.slice(start, end))) return false;
        start = end + 1;
    }
    return start > 0;
}

function reply(body) {
    return ContentService.createTextOutput(JSON.stringify(body)).setMimeType(ContentService.MimeType.JSON);
}

function doPost(event) {
    const folder = PropertiesService.getScriptProperties().getProperty(folderProperty);
    if (!folder) return reply({ error: "inbox_unavailable" });
    const { title = "", version = "" } = event.parameter || {};
    if (!/^[0-9A-F]{16}$/.test(title) || !/^[A-Za-z0-9][A-Za-z0-9 ._+()-]{0,63}$/.test(version)) {
        return reply({ error: "invalid_game" });
    }
    const data = event.postData;
    if (!data || data.type.split(";")[0] !== mime || data.length > maxBytes ||
        data.contents.length > maxBytes || !validRows(data.contents)) {
        return reply({ error: "invalid_list" });
    }

    const sha256 = Utilities.computeDigest(Utilities.DigestAlgorithm.SHA_256, data.contents, Utilities.Charset.UTF_8)
        .map(byte => (byte & 255).toString(16).padStart(2, "0")).join("");
    const name = `${title}--${version}--${sha256}.tsv`;
    const lock = LockService.getScriptLock();
    if (!lock.tryLock(5000)) return reply({ error: "inbox_busy" });
    try {
        const found = Drive.Files.list({ q: `'${folder}' in parents and name = '${name}' and trashed = false`,
            fields: "files(id)", pageSize: 1 });
        if (!found.files?.length) {
            Drive.Files.create({ name, parents: [folder], mimeType: mime }, Utilities.newBlob(data.contents, mime, name));
        }
        return reply({ sha256 });
    } finally {
        lock.releaseLock();
    }
}
