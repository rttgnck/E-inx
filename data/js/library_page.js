let books = [];
let activeFilter = "all";
let searchText = "";
let toastTimer = null;
let indexTimer = null;

const ICONS = {
  download:
    '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M10 4v9m-3.5-3.5L10 13l3.5-3.5M4 16h12"/></svg>',
  trash:
    '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6h12M8 3.5h4L13 6H7l1-2.5ZM6 6l.7 10h6.6L14 6M8.5 8.5v5M11.5 8.5v5"/></svg>',
};

function escapeHtml(value) {
  return value
    ? String(value).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[c])
    : "";
}

function escapeAttr(value) {
  return escapeHtml(value).replace(/"/g, "&quot;");
}

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  const unit = Math.min(3, Math.floor(Math.log(bytes) / Math.log(1024)));
  return (bytes / Math.pow(1024, unit)).toFixed(unit ? 1 : 0) + " " + ["B", "KB", "MB", "GB"][unit];
}

function formatTime(ms) {
  const minutes = Math.round((Number(ms) || 0) / 60000);
  if (minutes < 60) return minutes + "m";
  const hours = Math.floor(minutes / 60);
  const rem = minutes % 60;
  return rem ? hours + "h " + rem + "m" : hours + "h";
}

function fileName(path) {
  return String(path || "").split("/").pop() || "book";
}

function showToast(message, error) {
  const toast = document.getElementById("toast");
  if (!toast) return;
  clearTimeout(toastTimer);
  toast.textContent = message;
  toast.className = "toast show" + (error ? " error" : "");
  toastTimer = setTimeout(() => {
    toast.className = "toast";
  }, 2600);
}

function setProgress(text, count, percent, visible) {
  document.getElementById("progress").classList.toggle("show", Boolean(visible));
  document.getElementById("progressText").textContent = text || "";
  document.getElementById("progressCount").textContent = count || "";
  document.getElementById("progressFill").style.width = Math.max(0, Math.min(100, percent || 0)) + "%";
}

function bookMatchesFilter(book) {
  if (activeFilter === "all") return true;
  if (activeFilter === "reading") return Number(book.progress) > 0 && Number(book.progress) < 99;
  if (activeFilter === "finished") return Number(book.progress) >= 99;
  if (activeFilter === "untagged") return !book.tag;
  return book.folder === activeFilter || book.tag === activeFilter || book.type === activeFilter;
}

function bookMatchesSearch(book) {
  if (!searchText) return true;
  const haystack = [book.title, book.author, book.folder, book.tag, book.path].join(" ").toLowerCase();
  return haystack.includes(searchText);
}

function filteredBooks() {
  return books.filter((book) => bookMatchesFilter(book) && bookMatchesSearch(book));
}

function renderStats() {
  const total = books.length;
  const reading = books.filter((book) => Number(book.progress) > 0 && Number(book.progress) < 99).length;
  const pages = books.reduce((sum, book) => sum + (Number(book.pagesRead) || 0), 0);
  const time = books.reduce((sum, book) => sum + (Number(book.readingTimeMs) || 0), 0);
  document.getElementById("statBooks").textContent = total;
  document.getElementById("statRead").textContent = reading;
  document.getElementById("statPages").textContent = pages;
  document.getElementById("statTime").textContent = formatTime(time);
  document.getElementById("count").textContent = filteredBooks().length + " shown";
}

function uniqueValues(key) {
  return Array.from(new Set(books.map((book) => book[key]).filter(Boolean))).sort((a, b) =>
    String(a).localeCompare(String(b))
  );
}

function renderFilters() {
  const filters = [
    ["all", "All"],
    ["reading", "Reading"],
    ["finished", "Finished"],
    ["untagged", "Untagged"],
    ...uniqueValues("type").map((value) => [value, value]),
    ...uniqueValues("tag").slice(0, 8).map((value) => [value, value]),
    ...uniqueValues("folder").slice(0, 8).map((value) => [value, value]),
  ];
  document.getElementById("filters").innerHTML = filters
    .map(
      ([value, label]) =>
        '<button class="chip' +
        (activeFilter === value ? " active" : "") +
        '" type="button" data-filter="' +
        escapeAttr(value) +
        '">' +
        escapeHtml(label) +
        "</button>"
    )
    .join("");
}

function coverMarkup(book) {
  const title = escapeHtml(book.title || fileName(book.path));
  if (book.coverUrl) {
    return (
      '<div class="cover"><img src="' +
      escapeAttr(book.coverUrl) +
      '" alt=""><div class="progress-ring"><span style="width:' +
      Math.max(0, Math.min(100, Number(book.progress) || 0)) +
      '%"></span></div></div>'
    );
  }
  return (
    '<div class="cover blank">' +
    title +
    '<div class="progress-ring"><span style="width:' +
    Math.max(0, Math.min(100, Number(book.progress) || 0)) +
    '%"></span></div></div>'
  );
}

function renderBooks() {
  const list = filteredBooks();
  const grid = document.getElementById("grid");
  document.getElementById("empty").classList.toggle("show", list.length === 0);
  grid.innerHTML = list
    .map((book) => {
      const title = book.title || fileName(book.path);
      const author = book.author || book.folder || "";
      return (
        '<article class="book">' +
        coverMarkup(book) +
        '<div class="book-title">' +
        escapeHtml(title) +
        '</div><div class="book-author">' +
        escapeHtml(author) +
        '</div><div class="book-meta"><span>' +
        escapeHtml(book.type || "Book") +
        '</span><span>' +
        Math.round(Number(book.progress) || 0) +
        '%</span></div><div class="row-actions"><a class="icon-btn" title="Download" href="/download?path=' +
        encodeURIComponent(book.path) +
        '">' +
        ICONS.download +
        '</a><button class="icon-btn danger" title="Delete" type="button" data-delete="' +
        escapeAttr(book.path) +
        '">' +
        ICONS.trash +
        "</button></div></article>"
      );
    })
    .join("");
  renderStats();
}

function renderNotice(data) {
  const notice = document.getElementById("notice");
  if (!data.indexed) {
    notice.textContent = "Library index is missing. Refresh will scan the device and build thumbnails.";
    notice.classList.add("show");
  } else if (data.indexing) {
    notice.textContent = "Indexing library: " + (data.current || 0) + " / " + (data.total || 0);
    notice.classList.add("show");
  } else {
    notice.classList.remove("show");
  }
}

async function loadLibrary() {
  const response = await fetch("/api/library", { cache: "no-store" });
  if (!response.ok) throw new Error((await response.text()) || "Could not load library");
  const data = await response.json();
  books = Array.isArray(data.books) ? data.books : [];
  renderNotice(data);
  renderFilters();
  renderBooks();
  if (data.indexing) scheduleIndexPoll();
}

async function refreshIndex() {
  const button = document.getElementById("refreshBtn");
  button.disabled = true;
  try {
    const response = await fetch("/api/library-index/refresh", { method: "POST" });
    if (!response.ok) throw new Error((await response.text()) || "Could not start indexing");
    showToast("Library refresh started", false);
    scheduleIndexPoll();
  } catch (error) {
    showToast(error.message, true);
  } finally {
    button.disabled = false;
  }
}

function scheduleIndexPoll() {
  clearTimeout(indexTimer);
  indexTimer = setTimeout(pollIndex, 850);
}

async function pollIndex() {
  try {
    const response = await fetch("/api/library-index/status", { cache: "no-store" });
    const status = await response.json();
    const total = Number(status.total) || 0;
    const current = Number(status.current) || 0;
    const percent = total ? (current / total) * 100 : 0;
    setProgress("Indexing library", current + "/" + total, percent, status.indexing);
    if (status.indexing) {
      scheduleIndexPoll();
    } else {
      setProgress("", "", 0, false);
      await loadLibrary();
    }
  } catch (error) {
    setProgress("", "", 0, false);
    showToast(error.message || "Index status unavailable", true);
  }
}

async function uploadFiles(files) {
  const accepted = Array.from(files || []).filter((file) => /\.(epub|txt|md|xtc|xtch)$/i.test(file.name));
  if (!accepted.length) {
    showToast("Choose EPUB, XTC, TXT, or Markdown books", true);
    return;
  }
  let completed = 0;
  const failures = [];
  for (let i = 0; i < accepted.length; i++) {
    const file = accepted[i];
    setProgress("Uploading " + file.name, i + 1 + "/" + accepted.length, (i / accepted.length) * 100, true);
    const form = new FormData();
    form.append("file", file, file.name);
    try {
      const response = await fetch("/upload?path=" + encodeURIComponent("/"), { method: "POST", body: form });
      if (!response.ok) throw new Error((await response.text()) || "Upload failed");
      completed++;
    } catch (error) {
      failures.push(file.name);
    }
  }
  setProgress("Upload complete", completed + "/" + accepted.length, 100, true);
  showToast(failures.length ? completed + " uploaded, " + failures.length + " failed" : completed + " uploaded", failures.length > 0);
  setTimeout(() => setProgress("", "", 0, false), 1400);
  await refreshIndex();
}

async function deleteBook(path) {
  const title = books.find((book) => book.path === path)?.title || fileName(path);
  if (!confirm('Delete "' + title + '" from the device?')) return;
  const form = new FormData();
  form.append("path", path);
  form.append("type", "file");
  const response = await fetch("/delete", { method: "POST", body: form });
  if (!response.ok) {
    showToast((await response.text()) || "Delete failed", true);
    return;
  }
  showToast("Deleted " + title, false);
  await refreshIndex();
}

document.getElementById("search").addEventListener("input", (event) => {
  searchText = event.target.value.trim().toLowerCase();
  renderBooks();
});

document.getElementById("filters").addEventListener("click", (event) => {
  const button = event.target.closest("[data-filter]");
  if (!button) return;
  activeFilter = button.dataset.filter;
  renderFilters();
  renderBooks();
});

document.getElementById("grid").addEventListener("click", (event) => {
  const button = event.target.closest("[data-delete]");
  if (button) deleteBook(button.dataset.delete);
});

document.getElementById("refreshBtn").addEventListener("click", refreshIndex);
document.getElementById("uploadInput").addEventListener("change", (event) => {
  uploadFiles(event.target.files);
  event.target.value = "";
});

window.addEventListener("load", () => {
  loadLibrary().catch((error) => showToast(error.message, true));
});
