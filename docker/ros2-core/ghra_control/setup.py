from setuptools import find_packages, setup
import os
from glob import glob

package_name = 'ghra_control'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        (os.path.join('share', package_name, 'launch'), glob('launch/*.py')),
    ],
    install_requires=['setuptools', 'paho-mqtt'],
    zip_safe=True,
    entry_points={
        'console_scripts': [
            'control_node = ghra_control.control_node:main',
            'mqtt_bridge = ghra_control.mqtt_bridge:main',
        ],
    },
)
