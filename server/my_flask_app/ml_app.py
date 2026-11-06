import os
import numpy as np
from flask import Flask, request, jsonify
from tensorflow.keras.models import load_model
from tensorflow.keras.applications.xception import preprocess_input
from PIL import Image
import io

# Flask setup
app = Flask(__name__)

# --- Model Loading ---
MODEL_PATH = 'model/testChiliMNV2.keras'
model = None
class_labels = ["healthy", "leaf curl", "leaf spot", "whitefly", "yellowish"]

# Load the model at startup
try:
    if os.path.exists(MODEL_PATH):
        model = load_model(MODEL_PATH)
        print("INFO: Model loaded successfully.")
    else:
        # This message will be printed to the console when you run ml_app.py
        print(f"WARNING: Model file not found at '{MODEL_PATH}'. The '/predict' endpoint will not work.")
except Exception as e:
    print(f"ERROR: Could not load model. {e}")


def preprocess_and_predict(image_file_stream):
    """
    Preprocesses an image from a file stream and returns the prediction.
    """
    if model is None:
        raise RuntimeError("Model is not loaded. Please ensure the model file exists and loaded correctly.")
        
    # Open image from stream
    image = Image.open(image_file_stream).convert("RGB")
    
    # Preprocess for the model
    image = image.resize((224, 224))
    image_array = np.expand_dims(np.array(image), axis=0)
    processed_image = preprocess_input(image_array)
    
    # Make prediction
    prediction = model.predict(processed_image)[0]
    predicted_index = np.argmax(prediction)
    predicted_label = class_labels[predicted_index]
    confidence_score = round(float(prediction[predicted_index]) * 100, 2)

    # Get all class scores, ensuring they are Python floats
    class_scores = {label: round(float(score) * 100, 2) for label, score in zip(class_labels, prediction)}

    return predicted_label, confidence_score, class_scores

@app.route('/predict', methods=['POST'])
def predict_api():
    """
    API endpoint to receive an image and return a classification result.
    This is called by the main app.py.
    """
    if 'file' not in request.files:
        return jsonify({'error': 'No file part in the request'}), 400
        
    uploaded_file = request.files['file']
    
    if uploaded_file.filename == '':
        return jsonify({'error': 'No file selected for uploading'}), 400

    try:
        # Process the image directly from the file stream
        label, confidence, scores = preprocess_and_predict(uploaded_file.stream)
        
        return jsonify({
            'prediction': label,
            'confidence': confidence,
            'scores': scores
        })
    except Exception as e:
        print(f"ERROR: during prediction. {e}")
        return jsonify({'error': str(e)}), 500

if __name__ == '__main__':
    # The service will run on port 5001 to not conflict with the main app
    print("Starting ML service on http://localhost:5001")
    app.run(debug=True, port=5001) 