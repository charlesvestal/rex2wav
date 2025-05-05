
console.log('waveform.js loaded');
const form = document.getElementById('upload-form');
const waveformDiv = document.getElementById('waveform');

form.addEventListener('submit', e => {
    e.preventDefault();
    waveformDiv.innerHTML = '';
    const fileInput = form.querySelector('input[type=file]');
    const file = fileInput.files[0];
    if (!file) {
        waveformDiv.textContent = 'Please select a file.';
        return;
    }
    const reader = new FileReader();
    reader.onload = evt => parseSlices(evt.target.result);
    reader.onerror = () => waveformDiv.textContent = 'Error reading file.';
    reader.readAsArrayBuffer(file);
});

function parseChunks(view, offset = 0, parentLen = view.byteLength) {
    const slices = [];
    const end = offset + parentLen;
    while (offset + 8 <= view.byteLength && offset + 8 <= end) {
        const marker = view.getUint32(offset, false);
        const length = view.getUint32(offset + 4, false);
        const pad = length + (length % 2);
        const contentStart = offset + 8;

        if (marker === 0x43415420) { // 'CAT '
            const childStart = contentStart + 4;
            const childLen = pad - 4;
            slices.push(...parseChunks(view, childStart, childLen));
        } else if (marker === 0x534C4345) { // 'SLCE'
            const startSample = view.getUint32(contentStart, false);
            const sliceLen = view.getUint32(contentStart + 4, false);
            // Skip length-1 degenerate slices
            if (sliceLen > 1) {
                slices.push({ start: startSample, length: sliceLen });
            }
        }
        offset += 8 + pad;
    }
    return slices;
}

function parseSlices(buffer) {
  const view = new DataView(buffer);
  // Gather SLCE-defined slices, skipping degenerate lengths
  const sl = parseChunks(view, 0, view.byteLength)
    .filter(s => s.length > 1);
  if (!sl.length) {
    waveformDiv.textContent = 'No slices found.';
    return;
  }
  // First slice: from 0 to first SLCE start
  const slices = [{ start: 0, length: sl[0].start }, ...sl];
  // Render
  waveformDiv.innerHTML = '<h2>Slice Points</h2>';
  const ul = document.createElement('ul');
  slices.forEach((slc, idx) => {
    const li = document.createElement('li');
    li.textContent = `Slice ${idx + 1}: start sample ${slc.start}, length ${slc.length}`;
    ul.appendChild(li);
  });
  waveformDiv.appendChild(ul);
}