/* Copyright 2018 Intel Corporation

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
------------------------------------------------------------------------------*/

#include <ctype.h>
#include <string.h>

#include <iostream>
#include <string>
#include <sstream>

#include <log4cxx/logger.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/level.h>

#include <sawtooth_sdk/sawtooth_sdk.h>
#include <sawtooth_sdk/exceptions.h>

#include <cryptopp/sha.h>
#include <cryptopp/filters.h>
#include <cryptopp/hex.h>

using namespace log4cxx;

static log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger
    ("SimpleWallet"));

static const std::string SIMPLE_WALLET_NAMESPACE = "simplewallet";

#define DEFAULT_VALIDATOR_URL "tcp://validator:4004"

// Helper function: To generate an SHA512 hash and return it as a hex
// encoded string.
static std::string SHA512(const std::string& message) {
    std::string digest;
    CryptoPP::SHA512 hash;

    CryptoPP::StringSource hasher(message, true,
        new CryptoPP::HashFilter(hash,
          new CryptoPP::HexEncoder (
             new CryptoPP::StringSink(digest), false)));

    return digest;
}

// Helper function: Tokenize std::string based on a delimiter
std::vector<std::string> split(const std::string& str, char delimiter)
{
   std::istringstream strStream(str);
   std::string token;
   std::vector<std::string> tokens;
   
   while (std::getline(strStream, token, delimiter))
   {
      tokens.push_back(token);
   }
   return tokens;
}

// Helper function: To extract Action str and value integer from given string
void strToActionAndValue(const std::string& str, std::string& action, uint32_t& value) {
     std::vector<std::string> vs = split(str, ','); 
     if (vs.size() != 2) {
         std::string error = "invalid no. of arguments: expected 2, got:"
             + std::to_string(vs.size()) + "\n";
         throw sawtooth::InvalidTransaction(error);
     } 
     action = vs[0];
     value = std::stoi(vs[1]);
}

// Handles the processing of SimpleWallet transactions
// This is the place where you implement your TF logic
class SimpleWalletApplicator:  public sawtooth::TransactionApplicator {
 public:
    SimpleWalletApplicator(sawtooth::TransactionUPtr txn,
        sawtooth::GlobalStateUPtr state) :
        TransactionApplicator(std::move(txn), std::move(state)) { };

    void Apply() {
        std::cout << "SimpleWalletApplicator::Apply\n";
        std::string wallet_user_pubkey = this->txn->header()->GetValue(
            sawtooth::TransactionHeaderSignerPublicKey);

        const std::string& raw_data = this->txn->payload();
        std::string action;
        uint32_t value;

        strToActionAndValue(raw_data, action, value);
        std::cout << "Got: " << action << " and " << value << "\n";

        if (action == "deposit") {
            this->makeDeposit(wallet_user_pubkey, value);
        } else if (action == "withdraw") {
            this->doWithdraw(wallet_user_pubkey, value);
        } else {
            std::string error = "invalid action: '" + action + "'";
            throw sawtooth::InvalidTransaction(error);
        }
    }

 private:
    std::string MakeAddress(const std::string& wallet_user_pubkey) {
        return SHA512(SIMPLE_WALLET_NAMESPACE).substr(0, 6) +
            SHA512(wallet_user_pubkey).substr(0, 64);
    }

    // Handle the SimpleWallet Deposit action
    // overflow and underflow cases are ignored for this example
    void makeDeposit(const std::string& wallet_user_pubkey,
                     const uint32_t& value) {

        auto address = this->MakeAddress(wallet_user_pubkey);
        LOG4CXX_DEBUG(logger, "SimpleWalletApplicator::makeDeposit Key: "
            << wallet_user_pubkey
            << " Address: " << address);

        uint32_t stored_value = 0;
        std::string stored_value_str;

        if (this->state->GetState(&stored_value_str, address)) {
            std::cout << "Stored value: " << stored_value_str << "\n";
            if (stored_value_str.length() != 0) {
                stored_value = std::stoi(stored_value_str);
            }
        }

        stored_value += value;

        // encode the value map back to string for storage.
        LOG4CXX_DEBUG(logger, "Storing " << value << " units");
        stored_value_str = std::to_string(stored_value);
        this->state->SetState(address, stored_value_str);
    }

    // Handle SimpleWallet Withdraw action.
    void doWithdraw(const std::string& wallet_user_pubkey,
                    const uint32_t& value) {

        auto address = this->MakeAddress(wallet_user_pubkey);

        LOG4CXX_DEBUG(logger, "SimpleWalletApplicator::doWithdraw Key: "
            << wallet_user_pubkey
            << " Address: " << address);

        uint32_t stored_value = 0;
        std::string stored_value_str;

        if(this->state->GetState(&stored_value_str, address)) {
            stored_value = std::stoi(stored_value_str);
        } else {
            std::string error = "Action was 'withdraw', but address"
                " not found in state for Key: " + wallet_user_pubkey;
            throw sawtooth::InvalidTransaction(error);
        }

        if (stored_value > 0 && stored_value >= value) {
            stored_value -= value;
        } else {
            std::string error = "You don't have any sufficient balance"
                " to withdraw." + wallet_user_pubkey;
            throw sawtooth::InvalidTransaction(error);
        }

        // encode the value map back to string for storage.
        LOG4CXX_DEBUG(logger, "Storing " << stored_value << " units");
        stored_value_str = std::to_string(stored_value);
        this->state->SetState(address, stored_value_str);
    }
};

// Define the SimpleWallet Handler to register with the transaction processor
// It sets the versions and types of transactions that can be handled
class SimpleWalletHandler: public sawtooth::TransactionHandler {

public:
    //Generating a namespace prefix in the default constructor
    SimpleWalletHandler() {
        this->namespacePrefix = SHA512(SIMPLE_WALLET_NAMESPACE).substr(0, 6);
        LOG4CXX_DEBUG(logger, "namespace:" << this->namespacePrefix);
    }

    std::string transaction_family_name() const {
        return std::string(SIMPLE_WALLET_NAMESPACE);
    }

    std::list<std::string> versions() const {
        return { "1.0" };
    }

    std::list<std::string> namespaces() const {
        return { namespacePrefix };
    }

    sawtooth::TransactionApplicatorUPtr GetApplicator(
            sawtooth::TransactionUPtr txn,
            sawtooth::GlobalStateUPtr state) {
        return sawtooth::TransactionApplicatorUPtr(
            new SimpleWalletApplicator(std::move(txn), std::move(state)));
    }
private:
    std::string namespacePrefix;
};


void usage(bool doExit = false, int exitCode = 1) {

    std::cout << "Usage" << std::endl;
    std::cout << "simple-wallet-tp [options] [connect_string]" << std::endl;
    std::cout << "  -h, --help - print this message" << std::endl;

    std::cout <<
    "  connect_string - connect string to validator in format tcp://host:port"
    << std::endl;

    if (doExit) {
        exit(exitCode);
    }
}

void parseArgs(int argc, char** argv, std::string& connectToValidatorUrl) {

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            usage(true, 0);
        } else if (i != (argc - 1)) {
            std::cout << "Invalid command line argument:" << arg << std::endl;
            usage(true);
        } else {
            connectToValidatorUrl = arg;
        }
    }
}

int main(int argc, char** argv) {

    try {
        std::string connectToValidatorUrl = DEFAULT_VALIDATOR_URL;

        parseArgs(argc, argv, connectToValidatorUrl);

        // Set up a simple configuration that logs on the console.
        BasicConfigurator::configure();
        
        // Set logging verbosity to max
        logger->setLevel(Level::getAll());

        // Create a transaction processor

        // connect to validator at connectToValidatorUrl
        sawtooth::TransactionProcessorUPtr processor(
            sawtooth::TransactionProcessor::Create(connectToValidatorUrl));

        // create a transaction handler for our SimpleWallet TF
        sawtooth::TransactionHandlerUPtr transaction_handler(
            new SimpleWalletHandler());

        // register the transaction handler with validator
        processor->RegisterHandler(
            std::move(transaction_handler));

        // run the transaction processor
        processor->Run();

        return 0;
    } catch(std::exception& e) {
        std::cerr << "Unexpected exception exiting: " << std::endl;
        std::cerr << e.what() << std::endl;
    } catch(...) {
        std::cerr << "Exiting due to unknown exception." << std::endl;
    }

    return -1;
}
