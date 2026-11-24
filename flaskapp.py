"""Starts the server: database (database/) + staff website (website/).

Run from anywhere:  python flaskapp.py
"""
import os
from pathlib import Path

from flask import Flask

from database.extensions import db

BASE_DIR = Path(__file__).resolve().parent

# instance_path: keep the database file in database/instance/rehab.db
# static_folder=None: the website/ folder serves its own CSS
app = Flask(__name__, instance_path=str(BASE_DIR / "database" / "instance"), static_folder=None)

# Configure the database
app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///rehab.db"
app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False
app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY", "dev-only-secret-change-me")

# Initialize SQLAlchemy
db.init_app(app)

# Import database models
from database import models  # noqa: E402,F401  (registers the tables)

# Create database tables
with app.app_context():
    db.create_all()

# Staff website pages + station API (website/)
from website.routes import site  # noqa: E402
from website.api import api  # noqa: E402
app.register_blueprint(site)
app.register_blueprint(api)

if __name__ == "__main__":
    # 0.0.0.0 so the UNO Q (and other devices on the network) can reach the server
    app.run(host="0.0.0.0", port=5000, debug=True)
