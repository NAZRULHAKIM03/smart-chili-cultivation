from flask import Flask, render_template, request, redirect, url_for, session, flash, jsonify, send_from_directory, send_file
import random 
import sqlite3
from datetime import datetime, timedelta
import hashlib
from pytz import timezone
import os
import base64
from PIL import Image
import io
import numpy as np
import requests
from io import BytesIO
from reportlab.lib.pagesizes import letter, landscape
from reportlab.pdfgen import canvas
from reportlab.platypus import SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet

# Set timezone for Malaysia
MY_TZ = timezone('Asia/Kuala_Lumpur')

# Create necessary directories
UPLOAD_FOLDER = 'static/uploads'
if not os.path.exists(UPLOAD_FOLDER):
    os.makedirs(UPLOAD_FOLDER)

#Initialize database
def get_db_connection():
    conn = sqlite3.connect('chilli.db')
    conn.row_factory = sqlite3.Row # Access columns by name
    return conn

# Create tables if not exists
def init_db():
    conn = get_db_connection()
    
    # Create tables if not exists
    conn.execute('''
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            temperature REAL,
            humidity REAL,
            pressure REAL,
            light REAL,
            pump_irrigation BOOLEAN,
            pump_fertilizer BOOLEAN,
            npk_soil_moisture REAL,
            npk_soil_temperature REAL,
            npk_nitrogen INTEGER,
            npk_phosphorus INTEGER,
            npk_potassium INTEGER,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Image data table
    conn.execute('''
        CREATE TABLE IF NOT EXISTS plant_images (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            image_path TEXT NOT NULL,
            ml_result TEXT,
            confidence REAL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Users table for authentication
    conn.execute('''
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Create default users (all with password: chili123)
    default_password_hash = hashlib.sha256('chili123'.encode()).hexdigest()
    try:
        conn.execute('DELETE FROM users WHERE username = ?', ('admin',))  # Remove old admin user
        
        # List of usernames to add
        usernames = ['Nazrul', 'Aishah', 'Lily', 'Roy']
        
        # Add each user
        for username in usernames:
            conn.execute('''
                INSERT OR REPLACE INTO users (username, password_hash)
                VALUES (?, ?)
            ''', (username, default_password_hash))
            
        conn.commit()
    except sqlite3.Error as e:
        print(f"Database error: {e}")
    finally:
        conn.close()

# Call this when app starts
init_db()

app = Flask(__name__)
app.secret_key = 'your-secret-key-change-this-in-production'
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

# Global variable to store latest sensor readings
latest_readings = {
    'temperature': None,
    'humidity': None,
    'pressure': None,
    'light': None,
    'pump_irrigation': False,
    'pump_fertilizer': False,
    'npk_soil_moisture': None,
    'npk_soil_temperature': None,
    'npk_nitrogen': None,
    'npk_phosphorus': None,
    'npk_potassium': None,
    'timestamp': None
}

latest_image = {
    'image_path': None,
    'ml_result': None,
    'confidence': None,
    'timestamp': None
}

def hash_password(password):
    return hashlib.sha256(password.encode()).hexdigest()

def verify_password(password, password_hash):
    return hash_password(password) == password_hash

def login_required(f):
    def decorated_function(*args, **kwargs):
        if 'user_id' not in session:
            return redirect(url_for('login'))
        return f(*args, **kwargs)
    decorated_function.__name__ = f.__name__
    return decorated_function

@app.route('/api/sensor-data', methods=['POST'])
def receive_sensor_data():
    try:
        data = request.get_json()
        
        # Get current time in Malaysia timezone
        current_time = datetime.now(MY_TZ)
        
        # Update latest readings
        global latest_readings
        latest_readings.update({
            'temperature': data.get('temperature'),
            'humidity': data.get('humidity'),
            'pressure': data.get('pressure'),
            'light': data.get('light'),
            'pump_irrigation': data.get('pump_irrigation', False),
            'pump_fertilizer': data.get('pump_fertilizer', False),
            'npk_soil_moisture': data.get('npk_soil_moisture'),
            'npk_soil_temperature': data.get('npk_soil_temperature'),
            'npk_nitrogen': data.get('npk_nitrogen'),
            'npk_phosphorus': data.get('npk_phosphorus'),
            'npk_potassium': data.get('npk_potassium'),
            'timestamp': current_time
        })
        
        # Store in database
        conn = get_db_connection()
        conn.execute('''
            INSERT INTO sensor_readings
            (temperature, humidity, pressure, light, pump_irrigation, pump_fertilizer,
             npk_soil_moisture, npk_soil_temperature, npk_nitrogen, npk_phosphorus, npk_potassium,
             timestamp)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ''', (
            data.get('temperature'),
            data.get('humidity'),
            data.get('pressure'),
            data.get('light'),
            data.get('pump_irrigation', False),
            data.get('pump_fertilizer', False),
            data.get('npk_soil_moisture'),
            data.get('npk_soil_temperature'),
            data.get('npk_nitrogen'),
            data.get('npk_phosphorus'),
            data.get('npk_potassium'),
            current_time.strftime('%Y-%m-%d %H:%M:%S')
        ))
        conn.commit()
        conn.close()
        
        return jsonify({'status': 'success'})
    except Exception as e:
        print(f"Error in receive_sensor_data: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        username = request.form['username']
        password = request.form['password']
        
        conn = get_db_connection()
        user = conn.execute(
            'SELECT * FROM users WHERE username = ?', (username,)
        ).fetchone()
        conn.close()
        
        if user and verify_password(password, user['password_hash']):
            session['user_id'] = user['id']
            session['username'] = user['username']
            flash('Login successful!', 'success')
            return redirect(url_for('dashboard'))
        else:
            flash('Invalid username or password!', 'error')
    
    return render_template('login.html')

@app.route('/logout')
def logout():
    session.clear()
    flash('You have been logged out.', 'info')
    return redirect(url_for('login'))

# Helper to call ML microservice
def get_ml_prediction(image_path):
    url = 'http://localhost:5001/predict'
    try:
        with open(image_path, 'rb') as f:
            files = {'file': f}
            response = requests.post(url, files=files)
        if response.status_code == 200:
            data = response.json()
            return data.get('prediction', 'Unknown'), data.get('confidence', 0.0)
        else:
            print(f"ML service error: {response.text}")
            return 'Unknown', 0.0
    except Exception as e:
        print(f"Error calling ML service: {e}")
        return 'Unknown', 0.0

@app.route('/api/upload-image', methods=['POST'])
def upload_image():
    try:
        data = request.get_json()
        image_data = data.get('image')  # Base64 encoded image
        
        # Get current time in Malaysia timezone
        current_time = datetime.now(MY_TZ)
        timestamp_str = current_time.strftime('%Y%m%d_%H%M%S')
        
        # Decode base64 image
        image_bytes = base64.b64decode(image_data)
        image = Image.open(io.BytesIO(image_bytes))
        
        # Save image
        image_filename = f'plant_{timestamp_str}.jpg'
        image_path = os.path.join(app.config['UPLOAD_FOLDER'], image_filename)
        image.save(image_path)
        
        # ML prediction via microservice
        ml_result, confidence = get_ml_prediction(image_path)

        # Store in database
        conn = get_db_connection()
        conn.execute('''
            INSERT INTO plant_images (image_path, ml_result, confidence, timestamp)
            VALUES (?, ?, ?, ?)
        ''', (image_filename, ml_result, confidence, current_time.strftime('%Y-%m-%d %H:%M:%S')))
        conn.commit()
        conn.close()

        # Update latest image info
        global latest_image
        latest_image.update({
            'image_path': image_filename,
            'ml_result': ml_result,
            'confidence': confidence,
            'timestamp': current_time
        })
        
        return jsonify({
            'status': 'success',
            'result': ml_result,
            'confidence': confidence
        })
    except Exception as e:
        print(f"Error in upload_image: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/')
@login_required
def dashboard():
    # Get the last 10 full hours
    now = datetime.now(MY_TZ).replace(minute=0, second=0, microsecond=0)
    hours = [now - timedelta(hours=i) for i in range(10)]
    hours.reverse()  # Oldest first

    conn = get_db_connection()
    history = []
    for hour in hours:
        hour_start = hour
        hour_end = hour + timedelta(hours=1)
        hour_start_str = hour_start.strftime('%Y-%m-%d %H:%M:%S')
        hour_end_str = hour_end.strftime('%Y-%m-%d %H:%M:%S')
        row = conn.execute('''
            SELECT * FROM sensor_readings
            WHERE timestamp >= ? AND timestamp < ?
            ORDER BY timestamp DESC
            LIMIT 1
        ''', (hour_start_str, hour_end_str)).fetchone()
        if row:
            history.append(dict(row))
        else:
            history.append({'timestamp': hour_start_str})

    # Get latest image data
    latest_plant_image = conn.execute('''
        SELECT * FROM plant_images
        ORDER BY timestamp DESC
        LIMIT 1
    ''').fetchone()
    conn.close()

    # Convert and format history
    for entry in history:
        dt = datetime.fromisoformat(entry['timestamp']).astimezone(MY_TZ)
        entry['timestamp_display'] = dt.strftime("%d %b %I:%M %p")
        entry['timestamp_iso'] = dt.isoformat()
        # Format values to match real-time display
        entry['temperature'] = f"{entry['temperature']:.1f}" if entry.get('temperature') is not None else '—'
        entry['humidity'] = f"{entry['humidity']:.1f}" if entry.get('humidity') is not None else '—'
        entry['pressure'] = f"{entry['pressure']:.1f}" if entry.get('pressure') is not None else '—'
        entry['light'] = f"{entry['light']:.1f}" if entry.get('light') is not None else '—'
        entry['npk_soil_moisture'] = f"{entry['npk_soil_moisture']:.1f}" if entry.get('npk_soil_moisture') is not None else '—'
        entry['npk_soil_temperature'] = f"{entry['npk_soil_temperature']:.1f}" if entry.get('npk_soil_temperature') is not None else '—'
        entry['npk_nitrogen'] = str(entry['npk_nitrogen']) if entry.get('npk_nitrogen') is not None else '—'
        entry['npk_phosphorus'] = str(entry['npk_phosphorus']) if entry.get('npk_phosphorus') is not None else '—'
        entry['npk_potassium'] = str(entry['npk_potassium']) if entry.get('npk_potassium') is not None else '—'
        entry['pump_irrigation'] = entry.get('pump_irrigation', False)
        entry['pump_fertilizer'] = entry.get('pump_fertilizer', False)

    # After building the history list, reverse it so the latest is at the top
    history.reverse()

    # Determine if the latest reading is recent (within 2 minutes)
    latest_timestamp = latest_readings.get('timestamp')
    show_latest = False
    if latest_timestamp:
        now = datetime.now(MY_TZ)
        if isinstance(latest_timestamp, str):
            try:
                latest_dt = datetime.fromisoformat(latest_timestamp)
            except Exception:
                latest_dt = now
        else:
            latest_dt = latest_timestamp
        # If the latest reading is within the last 2 minutes
        if (now - latest_dt).total_seconds() <= 120:
            show_latest = True

    def dash_if_old(val):
        return val if show_latest and val is not None else '—'

    # Pass current date for date picker default
    now_str = datetime.now(MY_TZ).strftime('%Y-%m-%d')

    # Prepare last updated timestamp for display
    last_updated = '—'
    if latest_timestamp:
        try:
            if isinstance(latest_timestamp, str):
                dt = datetime.fromisoformat(latest_timestamp).astimezone(MY_TZ)
            else:
                dt = latest_timestamp.astimezone(MY_TZ)
            last_updated = dt.strftime('%d %b %Y, %I:%M %p')
        except Exception:
            pass

    return render_template('dashboard.html',
                        temperature=dash_if_old(latest_readings['temperature']),
                        humidity=dash_if_old(latest_readings['humidity']),
                        pressure=dash_if_old(latest_readings['pressure']),
                        light=dash_if_old(latest_readings['light']),
                        pump_irrigation=latest_readings['pump_irrigation'] if show_latest else None,
                        pump_fertilizer=latest_readings['pump_fertilizer'] if show_latest else None,
                        npk_soil_moisture=dash_if_old(latest_readings['npk_soil_moisture']),
                        npk_soil_temperature=dash_if_old(latest_readings['npk_soil_temperature']),
                        npk_nitrogen=dash_if_old(latest_readings['npk_nitrogen']),
                        npk_phosphorus=dash_if_old(latest_readings['npk_phosphorus']),
                        npk_potassium=dash_if_old(latest_readings['npk_potassium']),
                        history=history,
                        username=session.get('username'),
                        latest_image=latest_plant_image if latest_plant_image else None,
                        now=now_str,
                        last_updated=last_updated)

@app.route('/export/pdf')
@login_required
def export_pdf():
    # Get date from query parameter
    date_str = request.args.get('date')
    if not date_str:
        return 'Date is required', 400
    try:
        day = datetime.strptime(date_str, '%Y-%m-%d').replace(tzinfo=MY_TZ)
    except Exception:
        return 'Invalid date format', 400

    # Prepare 24 hours for the selected day
    hours = [day.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(hours=i) for i in range(24)]
    conn = get_db_connection()
    readings = []
    for hour in hours:
        hour_start = hour
        hour_end = hour + timedelta(hours=1)
        hour_start_str = hour_start.strftime('%Y-%m-%d %H:%M:%S')
        hour_end_str = hour_end.strftime('%Y-%m-%d %H:%M:%S')
        row = conn.execute('''
            SELECT * FROM sensor_readings
            WHERE timestamp >= ? AND timestamp < ?
            ORDER BY timestamp DESC
            LIMIT 1
        ''', (hour_start_str, hour_end_str)).fetchone()
        if row:
            readings.append(dict(row))
        else:
            readings.append({'timestamp': hour_start_str})
    conn.close()

    # Prepare table data
    headers = ['Time', 'Temp (°C)', 'Humidity (%)', 'Pressure (hPa)', 'Light (lux)', 'Soil Moisture (%)', 'Soil Temp (°C)', 'N (mg/kg)', 'P (mg/kg)', 'K (mg/kg)']
    data = [headers]
    for entry in readings:
        dt = datetime.fromisoformat(entry['timestamp']).astimezone(MY_TZ)
        # Show actual reading time (HH:MM) instead of just hour slot
        time_str = dt.strftime('%H:%M')
        row = [
            time_str,
            f"{entry['temperature']:.1f}" if entry.get('temperature') is not None else '—',
            f"{entry['humidity']:.1f}" if entry.get('humidity') is not None else '—',
            f"{entry['pressure']:.1f}" if entry.get('pressure') is not None else '—',
            f"{entry['light']:.1f}" if entry.get('light') is not None else '—',
            f"{entry['npk_soil_moisture']:.1f}" if entry.get('npk_soil_moisture') is not None else '—',
            f"{entry['npk_soil_temperature']:.1f}" if entry.get('npk_soil_temperature') is not None else '—',
            str(entry['npk_nitrogen']) if entry.get('npk_nitrogen') is not None else '—',
            str(entry['npk_phosphorus']) if entry.get('npk_phosphorus') is not None else '—',
            str(entry['npk_potassium']) if entry.get('npk_potassium') is not None else '—',
        ]
        data.append(row)

    # Generate PDF with table and borders
    buffer = BytesIO()
    doc = SimpleDocTemplate(buffer, pagesize=landscape(letter), rightMargin=20, leftMargin=20, topMargin=30, bottomMargin=20)
    elements = []
    styles = getSampleStyleSheet()
    elements.append(Paragraph(f'<b>Sensor Report for {date_str}</b>', styles['Title']))
    elements.append(Spacer(1, 12))
    elements.append(Paragraph('Recent Sensor Readings (hourly)', styles['Heading2']))
    elements.append(Spacer(1, 8))
    col_widths = [40, 60, 70, 80, 65, 100, 85, 60, 60, 60]
    table = Table(data, colWidths=col_widths)
    table.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), colors.lightgrey),
        ('TEXTCOLOR', (0, 0), (-1, 0), colors.black),
        ('ALIGN', (0, 0), (-1, -1), 'CENTER'),
        ('FONTNAME', (0, 0), (-1, 0), 'Helvetica-Bold'),
        ('FONTSIZE', (0, 0), (-1, 0), 10),
        ('FONTSIZE', (0, 1), (-1, -1), 9),
        ('BOTTOMPADDING', (0, 0), (-1, 0), 8),
        ('GRID', (0, 0), (-1, -1), 0.5, colors.black),
        ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
    ]))
    elements.append(table)
    doc.build(elements)
    buffer.seek(0)
    return send_file(buffer, as_attachment=True, download_name=f'sensor_report_{date_str}.pdf', mimetype='application/pdf')

if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0')  # Allow external connections