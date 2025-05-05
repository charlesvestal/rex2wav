import os
from flask import Flask, request, redirect, url_for, render_template, jsonify, send_from_directory
from werkzeug.utils import secure_filename
from parse_rx2 import get_slices

UPLOAD_FOLDER = 'uploads'
ALLOWED_EXTENSIONS = {'rx2'}

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

if not os.path.exists(UPLOAD_FOLDER):
    os.makedirs(UPLOAD_FOLDER)


def allowed_file(filename):
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/upload', methods=['POST'])
def upload():
    # Handle file upload
    if 'file' not in request.files:
        return redirect(url_for('index'))
    file = request.files['file']
    if file.filename == '' or not allowed_file(file.filename):
        return redirect(url_for('index'))

    filename = secure_filename(file.filename)
    rx2_path = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(rx2_path)

    # Parse slice points
    slices = get_slices(rx2_path)

    return jsonify({
        'slices': [{'start': s[0], 'length': s[1]} for s in slices]
    })


@app.route('/uploads/<path:filename>')
def uploaded_file(filename):
    return send_from_directory(app.config['UPLOAD_FOLDER'], filename)


if __name__ == '__main__':
    app.run(debug=True)