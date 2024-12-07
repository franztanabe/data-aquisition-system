#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <fstream>
#include <mutex>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>

using boost::asio::ip::tcp;

// Definição do registro de log conforme enunciado
#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32];
    std::time_t timestamp;
    double value;
};
#pragma pack(pop)

static std::mutex file_mutex;

std::time_t string_to_time_t(const std::string& time_string) {
    std::tm tm = {};
    std::istringstream ss(time_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

std::string time_t_to_string(std::time_t time) {
    std::tm* tm = std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

class session
  : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket)
    : socket_(std::move(socket))
  {
  }

  void start()
  {
    read_message();
  }

private:
  void read_message()
  {
    auto self(shared_from_this());
    boost::asio::async_read_until(socket_, buffer_, "\r\n",
        [this, self](boost::system::error_code ec, std::size_t length)
        {
          if (!ec)
          {
            std::istream is(&buffer_);
            std::string message((std::istreambuf_iterator<char>(is)), {});
            // Remove \r\n do final da mensagem
            if (!message.empty() && message.back() == '\n') message.pop_back();
            if (!message.empty() && message.back() == '\r') message.pop_back();
            std::cout << "Received: " << message << std::endl;
            handle_message(message);
          }
        });
  }

  void handle_message(const std::string &message) {
    std::vector<std::string> tokens;
    {
        std::istringstream iss(message);
        std::string token;
        while(std::getline(iss, token, '|')) {
            tokens.push_back(token);
        }
    }

    if (tokens.empty()) {
        // Mensagem inválida
        read_message();
        return;
    }

    std::string command = tokens[0];
    if (command == "LOG") {
        // LOG|SENSOR_ID|DATA_HORA|LEITURA
        if (tokens.size() < 4) {
            read_message();
            return;
        }
        handle_log(tokens);
    } else if (command == "GET") {
        // GET|SENSOR_ID|NUMERO_DE_REGISTROS
        if (tokens.size() < 3) {
            write_message("ERROR|INVALID_SENSOR_ID\r\n");
        } else {
            handle_get(tokens);
        }
    } else {
        // Comando inválido
        write_message("ERROR|INVALID_SENSOR_ID\r\n");
    }
  }

  void handle_log(const std::vector<std::string> &tokens) {
    std::string sensor_id = tokens[1];
    std::string datetime = tokens[2];
    std::string leitura_str = tokens[3];

    LogRecord record;
    std::memset(record.sensor_id, 0, 32);
    std::strncpy(record.sensor_id, sensor_id.c_str(), 31);
    record.timestamp = string_to_time_t(datetime);
    record.value = std::stod(leitura_str);

    std::string filename = sensor_id + ".dat";

    {
        std::lock_guard<std::mutex> lock(file_mutex);
        std::ofstream fs(filename, std::ios::out | std::ios::app | std::ios::binary);
        fs.write(reinterpret_cast<const char*>(&record), sizeof(LogRecord));
    }
    // Não é especificado enviar resposta ao LOG, então apenas continuar
    read_message();
  }

  void handle_get(const std::vector<std::string> &tokens) {
    std::string sensor_id = tokens[1];
    std::string num_str = tokens[2];

    size_t requested = 0;
    try {
        requested = std::stoul(num_str);
    } catch(...) {
        write_message("ERROR|INVALID_SENSOR_ID\r\n");
        return;
    }

    std::string filename = sensor_id + ".dat";

    std::vector<LogRecord> records;
    size_t to_read = 0;

    {
        std::lock_guard<std::mutex> lock(file_mutex);
        std::ifstream fs(filename, std::ios::in | std::ios::binary);
        if(!fs.is_open()) {
            // Sensor inválido
            write_message("ERROR|INVALID_SENSOR_ID\r\n");
            return;
        }

        fs.seekg(0, std::ios::end);
        std::streampos file_size = fs.tellg();
        if(file_size < 0) {
            write_message("ERROR|INVALID_SENSOR_ID\r\n");
            return;
        }

        size_t total_records = file_size / sizeof(LogRecord);
        if (total_records == 0) {
            // Sem registros
            write_message("0\r\n");
            return;
        }

        to_read = std::min(requested, total_records);
        records.resize(to_read);
        fs.seekg((total_records - to_read)*sizeof(LogRecord), std::ios::beg);
        fs.read(reinterpret_cast<char*>(records.data()), to_read * sizeof(LogRecord));
    }

    std::ostringstream oss;
    oss << to_read;
    for (auto &r : records) {
        oss << ";" << time_t_to_string(r.timestamp) << "|" << r.value;
    }
    oss << "\r\n";

    write_message(oss.str());
  }

  void write_message(const std::string& message)
  {
    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(message),
        [this, self, message](boost::system::error_code ec, std::size_t /*length*/)
        {
          if (!ec)
          {
            // Após enviar a resposta ao GET, continuar lendo novas mensagens
            read_message();
          }
        });
  }

  tcp::socket socket_;
  boost::asio::streambuf buffer_;
};

class server
{
public:
  server(boost::asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
  {
    accept();
  }

private:
  void accept()
  {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket)
        {
          if (!ec)
          {
            std::make_shared<session>(std::move(socket))->start();
          }

          accept();
        });
  }

  tcp::acceptor acceptor_;
};

int run_server(int port)
{
  try {
    boost::asio::io_context io_context;
    server s(io_context, port);
    io_context.run();
  } catch(std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
