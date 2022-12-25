import QtQuick 2.0
import QtQuick.Controls 2.0
import QtQuick.Layouts 1.2
import GetThermal 1.0
import QtQml 2.2

Item {
    id: item1

    property UvcAcquisition acq: null
    property bool farenheitTemps: false

    width: 160
    height: 90

    function ktof(val) {
      return (1.8 * ktoc(val) + 32.0);
    }

    function ktoc(val) {
        return (val - 273.15);
    }

    function localtemp(k) {
        if (farenheitTemps) {
            return "" + ktof(k).toFixed(1) + "°F";
        }
        else {
            return "" + ktoc(k).toFixed(2) + "°C";
        }
    }

    ColumnLayout {
        anchors.fill: parent

        GroupBox {
            id: groupBox
            title: qsTr("IR Thermo.")
            Layout.fillWidth: true

            Label {
                id: labelObjectTemperature
                text: localtemp(acq.cci.irThermometerInKelvin)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Label {
            id: labelAmbientTemperature
            text: localtemp(acq.cci.irThermometerAmbientInKelvin) + " in sensor"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            fontSizeMode: Text.HorizontalFit
        }
    }
}
