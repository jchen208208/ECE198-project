from flask import Flask
from .extensions import db


def create_app():

    app = Flask(__name__)

    app.config["SECRET_KEY"] = "replace-with-a-random-secret"
    app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///rehab.db"
    app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False

    # Initialize database
    db.init_app(app)

    # Import models
    from . import models

    # Register pages
    from .pages import pages
    app.register_blueprint(pages)

    # Create database tables
    with app.app_context():
        db.create_all()

    return app